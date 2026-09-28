#include "astra_stream.h"
#include "cmx918_device.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <config.h>
#include <core.h>
#include <cstdlib>
#include <deque>
#include <future>
#include <gui/gui.h>
#include <gui/main_window.h>
#include <gui/tuner.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <module.h>
#include <mutex>
#include <signal_path/signal_path.h>
#include <thread>
#include <utils/flog.h>

SDRPP_MOD_INFO{"astra918_source",
               "Astra918 audio/CAT/SDR receiver",
               "Astra918 project",
               0,
               1,
               0,
               -1};
namespace {
ConfigManager config;
using Clock = std::chrono::steady_clock;
struct View {
  cmx::State state;
  cmx::Capabilities capabilities;
  bool connected = false, playing = false;
  std::string message = "Disconnected";
};
struct Request {
  enum Kind { Connect, Disconnect, Start, Stop, Set } kind;
  uint8_t cmd = 0;
  cmx::Bytes payload;
  std::string target;
  bool simulator = false;
};
class Source : public ModuleManager::Instance {
  std::string name, serial;
  bool simulator = false, selected = false, enabled = true;
  char address[128] = "127.0.0.1:7350";
  SourceManager::SourceHandler handler{};
  dsp::stream<dsp::complex_t> stream;
  std::thread control_worker, delivery_worker;
  std::atomic<bool> quitting{false}, delivering{false}, failed{false};
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<Request> requests;
  std::deque<cmx::IqBlock> blocks;
  View view;
  std::atomic<uint32_t> shown_generation{0};
  std::atomic<bool> restore_tuning{false};
  std::vector<cmx::DeviceInfo> devices;
  std::future<std::vector<cmx::DeviceInfo>> discovery;
  ImGuiContext *context = nullptr;
  ImGuiID frame_hook = 0, shutdown_hook = 0;
  uint64_t shown_center = 0;
  uint32_t shown_revision = UINT32_MAX;
  bool ui_initialized = false;
  int offset_draft = 0, low_draft = 100, high_draft = 3500;
  bool draft_dirty = false;
  Clock::time_point last_gain{};
  int gain_draft[4]{};
  bool gain_active[4]{};
  int capacitor_draft = 0;
  int capacitor_readback = -1;
  bool capacitor_active = false, capacitor_pending = false;
  Clock::time_point last_capacitor{};
  View snapshot() {
    std::lock_guard<std::mutex> lock(mutex);
    return view;
  }
  void message(const std::string &m) {
    std::lock_guard<std::mutex> lock(mutex);
    view.message = m;
  }
  void publish(cmx::Device &d, bool playing) {
    std::lock_guard<std::mutex> lock(mutex);
    view.state = d.state;
    view.capabilities = d.capabilities;
    view.connected = true;
    view.playing = playing;
  }
  void enqueue(Request r) {
    std::lock_guard<std::mutex> lock(mutex);
    if (requests.size() >= 64) {
      view.message = "Control queue full; wait for the pending operation";
      return;
    }
    // Coalesce consecutive updates to the same field, never across another
    // command.
    if (r.kind == Request::Set && !requests.empty() &&
        requests.back().kind == Request::Set && requests.back().cmd == r.cmd &&
        ((r.cmd != cmx::SetGain && r.cmd != cmx::UpdateGpio) ||
         requests.back().payload[0] == r.payload[0]))
      requests.back() = std::move(r);
    else
      requests.push_back(std::move(r));
    wake.notify_all();
  }
  void set(uint8_t cmd, const cmx::Bytes &p = {}) {
    enqueue({Request::Set, cmd, p, {}});
  }
  void clear_blocks() {
    std::lock_guard<std::mutex> lock(mutex);
    blocks.clear();
  }
  void control_loop() {
    std::unique_ptr<cmx::Device> device;
    bool playing = false;
    auto poll = Clock::now();
    auto report = Clock::now();
    std::atomic<uint64_t> samples{0};
    std::atomic<bool> reader_stop{true};
    std::thread reader;
    std::string reader_error; // protected by mutex
    auto stop_reader = [&] {
      reader_stop = true;
      if (reader.joinable())
        reader.join();
    };
    auto start_reader = [&] {
      reader_stop = false;
      {
        std::lock_guard<std::mutex> lock(mutex);
        reader_error.clear();
      }
      // Only this thread reads the I/Q endpoint. The control owner keeps the
      // Device alive until join, and owns all accesses to its state snapshot.
      reader = std::thread([&, active = device.get()] {
        cmx::AstraDecoder decoder;
        auto last_data = Clock::now();
        try {
          while (!reader_stop && !quitting) {
            auto bytes = active->read_iq();
            if (reader_stop || quitting)
              break;
            if (!bytes.empty()) {
              last_data = Clock::now();
              auto frames = decoder.feed(bytes);
              std::lock_guard<std::mutex> lock(mutex);
              for (auto &frame : frames) {
                samples += frame.samples.size();
                if (blocks.size() >= 128)
                  throw std::runtime_error("Host I/Q queue overflow");
                blocks.push_back(std::move(frame));
              }
              wake.notify_all();
            } else if (Clock::now() - last_data > std::chrono::seconds(2))
              throw std::runtime_error(
                  "I/Q timed out; CAT/audio remain independent");
          }
        } catch (const std::exception &e) {
          std::lock_guard<std::mutex> lock(mutex);
          reader_error = e.what();
          wake.notify_all();
        }
      });
    };
    while (!quitting) {
      Request request{};
      bool have = false;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!requests.empty()) {
          request = std::move(requests.front());
          requests.pop_front();
          have = true;
        }
      }
      try {
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (!reader_error.empty()) {
            auto error = std::move(reader_error);
            reader_error.clear();
            throw std::runtime_error(error);
          }
        }
        if (have) {
          if (request.kind == Request::Connect) {
            stop_reader();
            if (device)
              device->stop();
            device.reset();
            playing = false;
            delivering = false;
            clear_blocks();
            device = std::make_unique<cmx::Device>(
                request.simulator ? cmx::open_tcp(request.target)
                                  : cmx::open_usb(request.target));
            message("Connected; adopted receiver settings");
            publish(*device, false);
            poll = Clock::now();
          } else if (request.kind == Request::Disconnect) {
            stop_reader();
            if (device) {
              device->stop();
            }
            device.reset();
            playing = false;
            delivering = false;
            clear_blocks();
            std::lock_guard<std::mutex> lock(mutex);
            view.connected = false;
            view.playing = false;
            view.message = "Disconnected";
          } else if (device) {
            if (request.kind == Request::Start) {
              stop_reader();
              clear_blocks();
              device->start();
              samples = 0;
              report = Clock::now();
              flog::info("Astra918 I/Q started, generation {0}",
                         device->state.generation);
              playing = true;
              delivering = true;
              stream.clearWriteStop();
              start_reader();
            } else if (request.kind == Request::Stop) {
              stop_reader();
              playing = false;
              delivering = false;
              clear_blocks();
              device->stop();
            } else {
              device->state = cmx::State::decode(
                  device->command(request.cmd, request.payload));
              message(request.cmd == cmx::Save ? "Settings saved to receiver"
                                               : "Applied");
            }
            publish(*device, playing);
          }
        }
        if (device && Clock::now() - poll >= std::chrono::milliseconds(200)) {
          device->refresh();
          publish(*device, playing);
          poll = Clock::now();
          if (playing && Clock::now() - report >= std::chrono::seconds(10)) {
            const auto &s = device->state;
            flog::info("Astra918 samples={0} dial={1} offset={2} drops={3} "
                       "capture={4} usb={5} audio={6}/{7}/{8}",
                       samples.load(), s.requested, s.offset, s.dropped,
                       s.capture_faults, s.usb_faults, s.audio_under,
                       s.audio_over, s.audio_stalls);
            report = Clock::now();
          }
          if (playing &&
              (!device->state.streaming || !device->state.configured)) {
            stop_reader();
            playing = false;
            delivering = false;
            clear_blocks();
            publish(*device, false);
            message("Receiver stopped I/Q or reported a fault; change reference or retry");
          }
        }
        {
          std::unique_lock<std::mutex> lock(mutex);
          wake.wait_for(lock, std::chrono::milliseconds(10), [&] {
            return quitting || !requests.empty() || !reader_error.empty();
          });
        }
      } catch (const cmx::ProtocolError &e) {
        restore_tuning = true;
        if (e.code == cmx::ErrorCode::Command &&
            (request.cmd == cmx::TuneChannel || request.cmd == cmx::TuneCenter))
          message("Receiver firmware update required for SDR++ tuning modes");
        else
          message(std::string("Not applied: ") + e.what());
        if (device) {
          try {
            device->refresh();
            if (!device->state.configured) {
              stop_reader();
              playing = false;
              delivering = false;
              clear_blocks();
            }
            publish(*device, playing);
          } catch (...) {
            stop_reader();
            device.reset();
            failed = true;
            playing = false;
            delivering = false;
            clear_blocks();
            std::lock_guard<std::mutex> lock(mutex);
            view.connected = false;
            view.playing = false;
          }
        }
      } catch (const std::exception &e) {
        flog::error("Astra918 source: {0}", e.what());
        message(e.what());
        stop_reader();
        device.reset();
        playing = false;
        delivering = false;
        failed = true;
        clear_blocks();
        std::lock_guard<std::mutex> lock(mutex);
        view.connected = false;
        view.playing = false;
      }
    }
    stop_reader();
    if (device) {
      try {
        device->stop();
      } catch (...) {
      }
    }
  }
  void delivery_loop() {
    while (!quitting) {
      cmx::IqBlock block;
      {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_for(lock, std::chrono::milliseconds(20), [&] {
          return quitting || (!blocks.empty() && delivering);
        });
        if (quitting)
          break;
        if (blocks.empty() || !delivering)
          continue;
        // GUI tick publishes the matching frequency before samples reach SDR++.
        auto gen = shown_generation.load();
        if (blocks.front().generation != gen) {
          if (int32_t(blocks.front().generation - gen) < 0)
            blocks.pop_front();
          else
            wake.wait_for(lock, std::chrono::milliseconds(10));
          continue;
        }
        block = std::move(blocks.front());
        blocks.pop_front();
      }
      for (size_t i = 0; i < block.samples.size(); i++) {
        stream.writeBuf[i].re = block.samples[i].real();
        stream.writeBuf[i].im = block.samples[i].imag();
      }
      if (!stream.swap(int(block.samples.size())) && !quitting) {
        delivering = false;
      }
    }
  }
  void render_tuning(const cmx::State &s) {
    gui::waterfall.setCenterFrequency(double(s.center));
    const auto active = gui::waterfall.vfos.find(gui::waterfall.selectedVFO);
    const double local_offset = active == gui::waterfall.vfos.end()
                                    ? 0.0
                                    : active->second->generalOffset;
    gui::freqSelect.setFrequency(double(s.center) + local_offset);
    gui::freqSelect.frequencyChanged = false;
    shown_center = s.center;
    shown_revision = s.revision;
    shown_generation = s.generation;
    ui_initialized = true;
  }
  void tick() {
    if (discovery.valid() && discovery.wait_for(std::chrono::seconds(0)) ==
                                 std::future_status::ready) {
      try {
        devices = discovery.get();
        if (serial.empty() && !devices.empty())
          serial = devices.front().serial;
      } catch (const std::exception &e) {
        message(e.what());
      }
    }
    if (!selected)
      return;
    if (failed.exchange(false) && gui::mainWindow.isPlaying())
      gui::mainWindow.setPlayState(false);
    auto v = snapshot();
    if (!v.connected)
      return;
    if (restore_tuning.exchange(false))
      render_tuning(v.state);
    if (ui_initialized) {
      const double now_center = gui::waterfall.getCenterFrequency();
      // Only the waterfall center controls RF tuning. SDR++ Radio VFOs are
      // independent of the firmware USB audio channel in either tuning mode.
      if (now_center != double(shown_center)) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
          return;
        const double now_dial = now_center + v.state.offset;
        if (std::isfinite(now_center) && now_center >= 70000 &&
            now_center <= 170000000 && now_dial >= 70000 &&
            now_dial <= 170000000) {
          auto proposed = v.state;
          proposed.requested = uint64_t(std::llround(now_dial));
          proposed.center = uint64_t(std::llround(now_center));
          set(cmx::SetFrequency, cmx::integer(proposed.requested, 8));
          render_tuning(proposed);
          return;
        }
        message("Spectrum center or firmware audio frequency is out of range");
        render_tuning(v.state);
      }
    }
    if (!ui_initialized || v.state.revision != shown_revision ||
        v.state.generation != shown_generation)
      render_tuning(v.state);
    if (!draft_dirty) {
      offset_draft = v.state.offset;
      low_draft = v.state.audio_low;
      high_draft = v.state.audio_high;
    }
  }
  void connect() {
    ui_initialized = false;
    capacitor_readback = -1;
    capacitor_active = capacitor_pending = false;
    enqueue({Request::Connect, 0, {}, simulator ? address : serial, simulator});
  }
  void start() {
    auto v = snapshot();
    if (!v.connected)
      connect();
    enqueue({Request::Start, 0, {}, {}});
  }
  void stop() {
    delivering = false;
    stream.stopWriter();
    enqueue({Request::Stop, 0, {}, {}});
  }
  void gain(const char *label, int block, uint8_t value, int maximum) {
    int &n = gain_draft[block];
    if (!gain_active[block])
      n = value;
    ImGui::PushID(block);
    field(label);
    bool changed = ImGui::SliderInt("##gain", &n, 0, maximum);
    bool released = ImGui::IsItemDeactivatedAfterEdit();
    gain_active[block] = ImGui::IsItemActive();
    if ((changed &&
         Clock::now() - last_gain >= std::chrono::milliseconds(100)) ||
        released) {
      last_gain = Clock::now();
      set(cmx::SetGain, {uint8_t(block), uint8_t(n)});
    }
    ImGui::PopID();
  }
  static void field(const char *label) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
  }
  void menu() {
    auto v = snapshot();
    ImGui::TextWrapped("%s", v.message.c_str());
    ImGui::BeginDisabled(v.connected);
#ifndef NDEBUG
    ImGui::Checkbox("Offline simulator", &simulator);
    if (simulator) {
      field("Address");
      ImGui::InputText("##Address", address, sizeof(address));
    } else
#endif
    {
      field("Receiver");
      if (ImGui::BeginCombo("##Receiver", serial.empty() ? "Select receiver"
                                                         : serial.c_str())) {
        for (auto &d : devices) {
          if (ImGui::Selectable(d.label.c_str(), serial == d.serial))
            serial = d.serial;
        }
        ImGui::EndCombo();
      }
      if (ImGui::Button("Refresh") && !discovery.valid())
        discovery = std::async(std::launch::async, cmx::enumerate_usb);
    }
    ImGui::EndDisabled();
    if (ImGui::Button(v.connected ? "Disconnect" : "Connect")) {
      if (v.connected) {
        stop();
        enqueue({Request::Disconnect, 0, {}, {}});
      } else
        connect();
    }
    ImGui::TextWrapped("I/Q: 120 ksps; USB audio: 12 kHz mono");
    if (!v.connected)
      return;
    auto s = v.state;
    ImGui::Separator();
    ImGui::Text("Spectrum center: %.6f MHz", s.center / 1e6);
    ImGui::Text("Firmware USB audio: %.6f MHz", s.requested / 1e6);
    field("Firmware USB audio offset (Hz)");
    draft_dirty |=
        ImGui::InputInt("##Firmware USB audio offset", &offset_draft);
    if (ImGui::Button("Apply offset")) {
      const int64_t dial = int64_t(s.center) + offset_draft;
      if (dial >= 70000 && dial <= 170000000)
        set(cmx::TuneChannel, cmx::integer(uint64_t(dial), 8));
      else
        message("Firmware USB audio frequency is out of range");
      draft_dirty = false;
    }
    int mode = s.audio_mode == 2 ? 0 : 1;
    field("Firmware USB audio mode");
    if (ImGui::Combo("##USB audio mode", &mode, "USB\0LSB\0"))
      set(cmx::SetAudioMode, {uint8_t(mode ? 1 : 2)});
    field("Audio low cut (Hz)");
    draft_dirty |= ImGui::InputInt("##Audio low cut", &low_draft);
    field("Audio high cut (Hz)");
    draft_dirty |= ImGui::InputInt("##Audio high cut", &high_draft);
    if (ImGui::Button("Apply audio filter")) {
      if (low_draft >= 0 && high_draft <= 5000 && low_draft < high_draft) {
        auto p = cmx::integer(low_draft, 2);
        auto high = cmx::integer(high_draft, 2);
        p.insert(p.end(), high.begin(), high.end());
        set(cmx::SetAudioFilter, p);
      }
      draft_dirty = false;
    }
    ImGui::Separator();
    if (v.capabilities.reference_clock) {
      int reference = s.reference_clock;
      field("38.4 MHz reference");
      if (ImGui::Combo("##38.4 MHz reference", &reference, "Internal\0External\0"))
        set(cmx::SetReference, {uint8_t(reference)});
    }
    if (v.capabilities.logical_gpio) {
      ImGui::TextUnformatted("Logical GPIO (pins unassigned)");
      for (int i = 0; i < 8; ++i) {
        if (i % 4)
          ImGui::SameLine();
        bool enabled = (s.gpio & (1u << i)) != 0;
        std::string label = "GPIO" + std::to_string(i);
        if (ImGui::Checkbox(label.c_str(), &enabled)) {
          uint8_t mask = uint8_t(1u << i);
          set(cmx::UpdateGpio, {mask, uint8_t(enabled ? mask : 0)});
        }
      }
    }
    ImGui::Separator();
    int input = s.input;
    field("Antenna input");
    if (ImGui::Combo("##Antenna input", &input, "Auto\0LF\0HF\0VHF\0"))
      set(cmx::SetInput, {uint8_t(input)});
    bool rf = s.rf_mode == 0, iff = s.if_mode == 0;
    if (ImGui::Checkbox("Automatic RF gain", &rf))
      set(cmx::SetGainMode, {0, uint8_t(!rf)});
    if (ImGui::Checkbox("Automatic IF gain", &iff))
      set(cmx::SetGainMode, {1, uint8_t(!iff)});
    if (!rf) {
      if (s.resolved_input == 1) {
        gain("LF gain", 2, s.lf_gain, 15);
        gain("LF attenuation", 3, s.lf_attenuator, 15);
      } else
        gain("RF gain", 0, s.rf_gain, 38);
    }
    if (!iff)
      gain("IF gain", 1, s.if_gain, 31);
    if (capacitor_readback != s.lf_mf_capacitor && !capacitor_active &&
        !capacitor_pending)
      capacitor_draft = s.lf_mf_capacitor;
    capacitor_readback = s.lf_mf_capacitor;
    field("LF/MF capacitor (0-4095)");
    capacitor_pending |=
        ImGui::SliderInt("##LF/MF capacitor", &capacitor_draft, 0, 4095, "%d",
                         ImGuiSliderFlags_AlwaysClamp);
    capacitor_active = ImGui::IsItemActive();
    capacitor_draft = std::clamp(capacitor_draft, 0, 4095);
    if (capacitor_pending &&
        (!capacitor_active ||
         Clock::now() - last_capacitor >= std::chrono::milliseconds(100))) {
      set(cmx::SetLfMfCapacitor, cmx::integer(capacitor_draft, 2));
      capacitor_pending = false;
      last_capacitor = Clock::now();
    }
    ImGui::Text("Capacitance: %.1f pF",
                cmx::lf_mf_capacitance_pf(s.lf_mf_capacitor));
    if (ImGui::Button("Save to receiver"))
      set(cmx::Save);
    ImGui::SameLine();
    if (ImGui::Button("Retry receiver"))
      set(cmx::Retry);
    ImGui::Text("Revision %u; %s", s.revision,
                s.saved_revision == s.revision ? "saved" : "unsaved");
    ImGui::TextWrapped("IQ drops %u; capture %u; USB %u", s.dropped,
                       s.capture_faults, s.usb_faults);
    ImGui::TextWrapped("Audio underruns %u; overruns %u; stalls %u",
                       s.audio_under, s.audio_over, s.audio_stalls);
    ImGui::TextWrapped("120 kHz sample span; the receiver filters roll off "
                       "near the edges. Error %u",
                       s.error);
  }
  void install_hook() {
    if (context || !ImGui::GetCurrentContext())
      return;
    context = ImGui::GetCurrentContext();
    ImGuiContextHook h{};
    h.UserData = this;
    h.Type = ImGuiContextHookType_NewFramePost;
    h.Callback = [](ImGuiContext *, ImGuiContextHook *h) {
      try {
        static_cast<Source *>(h->UserData)->tick();
      } catch (const std::exception &e) {
        flog::error("Astra GUI: {0}", e.what());
      }
    };
    frame_hook = ImGui::AddContextHook(context, &h);
    h.Type = ImGuiContextHookType_Shutdown;
    h.Callback = [](ImGuiContext *, ImGuiContextHook *h) {
      static_cast<Source *>(h->UserData)->context = nullptr;
    };
    shutdown_hook = ImGui::AddContextHook(context, &h);
  }

public:
  static int self_test(const char *endpoint) {
    auto *ctx = ImGui::CreateContext();
    static dsp::stream<dsp::complex_t> dummy;
    sigpath::iqFrontEnd.init(
        &dummy, 120000, false, 1, false, 1024, 20, IQFrontEnd::BLACKMAN,
        [](void *) -> float * {
          static float buffer[1024];
          return buffer;
        },
        [](void *) {}, nullptr);
    auto *vfo = sigpath::vfoManager.createVFO("Astra test",
                                              ImGui::WaterfallVFO::REF_LOWER, 0,
                                              3000, 12000, 300, 5000, false);
    auto *secondary = sigpath::vfoManager.createVFO(
        "Other test", ImGui::WaterfallVFO::REF_CENTER, 20000, 3000, 12000, 300,
        5000, false);
    gui::waterfall.selectedVFO = "Astra test";
    gui::waterfall.setBandwidth(120000);
    gui::waterfall.setViewBandwidth(120000);
    int result = 0;
    {
      Source s("offline lifecycle");
      s.simulator = true;
      std::snprintf(s.address, sizeof(s.address), "%s", endpoint);
      s.selected = true;
      std::atomic<uint64_t> count{0};
      std::atomic<bool> consume{true};
      std::thread reader([&] {
        while (consume) {
          int n = s.stream.read();
          if (n < 0)
            break;
          count += unsigned(n);
          s.stream.flush();
        }
      });
      auto wait = [&](auto condition) {
        auto deadline = Clock::now() + std::chrono::seconds(6);
        while (!condition() && Clock::now() < deadline) {
          s.tick();
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!condition())
          throw std::runtime_error("Module lifecycle timeout: " +
                                   s.snapshot().message);
      };
      try {
        s.connect();
        wait([&] { return s.snapshot().connected; });
        s.tick();
        s.set(cmx::SetOffset, cmx::integer(10000, 4));
        wait([&] { return s.snapshot().state.offset == 10000; });
        s.tick();
        s.handler.startHandler(s.handler.ctx);
        wait([&] { return count > 12000; });
        const auto before = s.snapshot().state.requested;
        const auto fixed_center = s.snapshot().state.center;
        gui::waterfall.VFOMoveSingleClick = false;
        ImGui::GetIO().MouseDown[ImGuiMouseButton_Left] = true;
        tuner::normalTuning("Astra test", double(before + 2000));
        s.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        s.tick();
        if (s.snapshot().state.requested != before)
          throw std::runtime_error("VFO drag retuned before mouse release");
        ImGui::GetIO().MouseDown[ImGuiMouseButton_Left] = false;
        s.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        s.tick();
        if (sigpath::vfoManager.getOffset("Astra test") != 12000 ||
            s.snapshot().state.requested != before ||
            s.snapshot().state.offset != 10000 ||
            s.snapshot().state.center != fixed_center ||
            gui::waterfall.getCenterFrequency() != double(fixed_center))
          throw std::runtime_error("Local VFO tuning changed firmware audio");
        gui::waterfall.VFOMoveSingleClick = true;
        const auto new_center = fixed_center + 3000;
        tuner::centerTuning("Astra test", double(new_center));
        s.tick();
        wait([&] { return s.snapshot().state.center == new_center; });
        const auto new_dial = new_center + 10000;
        if (s.snapshot().state.requested != new_dial ||
            s.snapshot().state.offset != 10000 ||
            sigpath::vfoManager.getOffset("Astra test") != 0)
          throw std::runtime_error(
              "Center tuning did not preserve audio offset");
        if (cmx::simulator_cat(endpoint, "FA;") !=
            "FA" + std::string(11 - std::to_string(new_dial).size(), '0') +
                std::to_string(new_dial) + ";")
          throw std::runtime_error("Spectrum retune did not propagate to CAT");
        gui::waterfall.VFOMoveSingleClick = false;
        const auto dial = s.snapshot().state.requested;
        gui::waterfall.selectedVFO = "Other test";
        tuner::normalTuning("Other test", double(dial + 15000));
        s.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        s.tick();
        if (s.snapshot().state.requested != dial)
          throw std::runtime_error("Secondary VFO changed CAT");
        tuner::normalTuning("Other test", double(dial + 500000));
        const auto recentered = uint64_t(gui::waterfall.getCenterFrequency());
        s.tick();
        wait([&] { return s.snapshot().state.center == recentered; });
        s.tick();
        if (s.snapshot().state.requested != recentered + 10000 ||
            s.snapshot().state.offset != 10000 ||
            gui::waterfall.getCenterFrequency() !=
                double(s.snapshot().state.center))
          throw std::runtime_error(
              "Waterfall recenter did not retune receiver");
        gui::waterfall.selectedVFO = "Astra test";
        if (cmx::simulator_cat(endpoint, "FA00007074049;FA;") !=
            "FA00007074049;")
          throw std::runtime_error("CAT retune failed");
        wait([&] { return s.snapshot().state.requested == 7074049; });
        s.tick();
        if (gui::waterfall.getCenterFrequency() != 7064049 ||
            sigpath::vfoManager.getOffset("Astra test") != 0)
          throw std::runtime_error(
              "CAT did not propagate to spectrum independently of VFO");
        auto n = count.load();
        wait([&] { return count > n + 12000; });
        s.stop();
        wait([&] { return !s.snapshot().playing; });
        if (!s.snapshot().state.configured)
          throw std::runtime_error("Stop stopped audio capture");
        const auto offset_center = s.snapshot().state.center;
        s.set(cmx::TuneChannel, cmx::integer(offset_center - 10000, 8));
        wait([&] { return s.snapshot().state.offset == -10000; });
        if (s.snapshot().state.center != offset_center ||
            s.snapshot().state.requested != offset_center - 10000)
          throw std::runtime_error("Audio offset edit moved spectrum center");
        s.start();
        n = count.load();
        wait([&] { return count > n + 12000; });
        s.stop();
        wait([&] { return !s.snapshot().playing; });
        s.enqueue({Request::Disconnect, 0, {}, {}});
        wait([&] { return !s.snapshot().connected; });
        s.connect();
        wait([&] { return s.snapshot().connected; });
        if (s.snapshot().state.requested != offset_center - 10000)
          throw std::runtime_error("Reconnect overwrote receiver state");
        flog::info(
            "Astra module: independent VFOs, center plus audio offset, CAT "
            "synchronization, stream/stop/reconnect passed");
      } catch (const std::exception &e) {
        flog::error("Astra module test failed: {0}", e.what());
        result = 1;
      }
      consume = false;
      s.stream.stopReader();
      s.stream.stopWriter();
      reader.join();
    }
    sigpath::vfoManager.deleteVFO(secondary);
    sigpath::vfoManager.deleteVFO(vfo);
    ImGui::DestroyContext(ctx);
    return result;
  }
  explicit Source(std::string instance) : name("Astra918 " + instance) {
    config.acquire();
    if (config.conf.contains(name)) {
      auto &p = config.conf[name];
      serial = p.value("serial", std::string());
    }
    config.release();
#ifndef NDEBUG
    if (const char *sim = std::getenv("ASTRA918_SIMULATOR")) {
      simulator = true;
      std::snprintf(address, sizeof(address), "%s", sim);
    }
#endif
    handler.ctx = this;
    handler.stream = &stream;
    handler.selectHandler = [](void *p) {
      auto &s = *static_cast<Source *>(p);
      s.selected = true;
      s.ui_initialized = false;
      core::setInputSampleRate(120000);
    };
    handler.deselectHandler = [](void *p) {
      auto &s = *static_cast<Source *>(p);
      s.selected = false;
      s.stop();
      s.enqueue({Request::Disconnect, 0, {}, {}});
    };
    handler.menuHandler = [](void *p) { static_cast<Source *>(p)->menu(); };
    handler.startHandler = [](void *p) { static_cast<Source *>(p)->start(); };
    handler.stopHandler = [](void *p) { static_cast<Source *>(p)->stop(); };
    // The frame hook reads the final waterfall center after upstream tuning.
    handler.tuneHandler = [](double, void *) {};
    sigpath::sourceManager.registerSource(name, &handler);
    control_worker = std::thread([this] { control_loop(); });
    delivery_worker = std::thread([this] { delivery_loop(); });
    install_hook();
  }
  ~Source() override {
    if (context) {
      ImGui::RemoveContextHook(context, frame_hook);
      ImGui::RemoveContextHook(context, shutdown_hook);
    }
    quitting = true;
    delivering = false;
    stream.stopWriter();
    wake.notify_all();
    control_worker.join();
    delivery_worker.join();
    sigpath::sourceManager.unregisterSource(name);
    config.acquire();
    config.conf[name] = {{"serial", serial}};
    config.release(true);
  }
  void postInit() override { install_hook(); }
  void enable() override { enabled = true; }
  void disable() override {
    enabled = false;
    stop();
  }
  bool isEnabled() override { return enabled; }
};
} // namespace
MOD_EXPORT void _INIT_() {
  config.setPath(core::args["root"].s() + "/astra918_source_config.json");
  config.load(nlohmann::json::object());
  config.enableAutoSave();
}
MOD_EXPORT ModuleManager::Instance *_CREATE_INSTANCE_(std::string name) {
  return new Source(name);
}
MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance *p) { delete p; }
MOD_EXPORT void _END_() {
  config.disableAutoSave();
  config.save();
}
MOD_EXPORT int ASTRA918_SIM_SELF_TEST(const char *endpoint) {
  return Source::self_test(endpoint);
}
