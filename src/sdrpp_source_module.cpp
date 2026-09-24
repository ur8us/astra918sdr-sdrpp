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
  std::string name, linked_vfo, serial;
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
  uint64_t shown_dial = 0, shown_center = 0;
  uint32_t shown_revision = UINT32_MAX;
  bool ui_initialized = false;
  double dial_draft = 14200000;
  int offset_draft = 0, low_draft = 100, high_draft = 3500;
  bool draft_dirty = false;
  Clock::time_point last_gain{};
  int gain_draft[4]{};
  bool gain_active[4]{};
  int capacitor_draft = 0;
  bool capacitor_dirty = false;
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
        (r.cmd != cmx::SetGain || requests.back().payload[0] == r.payload[0]))
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
              (!device->state.streaming || !device->state.configured))
            throw std::runtime_error(
                "Receiver stopped I/Q or reported a fault");
        }
        {
          std::unique_lock<std::mutex> lock(mutex);
          wake.wait_for(lock, std::chrono::milliseconds(10), [&] {
            return quitting || !requests.empty() || !reader_error.empty();
          });
        }
      } catch (const cmx::ProtocolError &e) {
        restore_tuning = true;
        message(std::string("Not applied: ") + e.what());
        if (device) {
          try {
            device->refresh();
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
  void choose_vfo() {
    if (!linked_vfo.empty() && gui::waterfall.vfos.count(linked_vfo))
      return;
    if (gui::waterfall.vfos.size() == 1)
      linked_vfo = gui::waterfall.vfos.begin()->first;
  }
  void render_tuning(const cmx::State &s) {
    gui::waterfall.setCenterFrequency(double(s.center));
    if (!linked_vfo.empty() && gui::waterfall.vfos.count(linked_vfo))
      sigpath::vfoManager.setOffset(linked_vfo, double(s.offset));
    if (gui::waterfall.selectedVFO == linked_vfo ||
        gui::waterfall.selectedVFO.empty()) {
      gui::freqSelect.setFrequency(s.requested);
      gui::freqSelect.frequencyChanged = false;
    }
    shown_dial = s.requested;
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
    choose_vfo();
    if (restore_tuning.exchange(false))
      render_tuning(v.state);
    if (ui_initialized) {
      double now_center = gui::waterfall.getCenterFrequency();
      const auto active = gui::waterfall.selectedVFO;
      if (now_center != double(shown_center) && !active.empty() &&
          active != linked_vfo && gui::waterfall.vfos.count(active)) {
        // Upstream auto-recenters the LO when a secondary VFO is dragged out
        // of view. Keep that action local: only the linked channel owns CAT.
        auto *other = gui::waterfall.vfos.at(active);
        const double limit = std::max(0.0, 60000.0 - other->bandwidth);
        const double offset =
            std::clamp(now_center + other->generalOffset - double(shown_center),
                       -limit, limit);
        render_tuning(v.state);
        sigpath::vfoManager.setOffset(active, offset);
        gui::freqSelect.setFrequency(double(shown_center) + offset);
        gui::freqSelect.frequencyChanged = false;
        now_center = double(shown_center);
        message("Secondary VFO limited to this spectrum; select the linked VFO "
                "to retune");
      }
      double now_dial = now_center + v.state.offset;
      if (!linked_vfo.empty() && gui::waterfall.vfos.count(linked_vfo))
        now_dial =
            now_center + gui::waterfall.vfos.at(linked_vfo)->generalOffset;
      // Inspect completed UI changes before applying asynchronous readbacks.
      if (std::isfinite(now_dial) &&
          std::llround(now_dial) != int64_t(shown_dial)) {
        // Keep the spectrum fixed while the mouse places the VFO. Recentring
        // under a held cursor makes upstream treat that same cursor position
        // as another tune every frame. Apply the completed gesture once.
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
          return;
        if (now_dial >= 70000 && now_dial <= 130000000) {
          auto proposed = v.state;
          proposed.requested = uint64_t(std::llround(now_dial));
          auto center = int64_t(proposed.requested) - proposed.offset;
          if (center >= 70000 && center <= 130000000) {
            proposed.center = uint64_t(center);
            set(cmx::SetFrequency, cmx::integer(proposed.requested, 8));
            render_tuning(proposed);
            return;
          }
        }
        render_tuning(v.state);
      }
    }
    if (!ui_initialized || v.state.revision != shown_revision ||
        v.state.generation != shown_generation)
      render_tuning(v.state);
    if (!draft_dirty) {
      dial_draft = double(v.state.requested);
      offset_draft = v.state.offset;
      low_draft = v.state.audio_low;
      high_draft = v.state.audio_high;
    }
  }
  void connect() {
    ui_initialized = false;
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
    ImGui::Checkbox("Offline simulator", &simulator);
    if (simulator) {
      field("Address");
      ImGui::InputText("##Address", address, sizeof(address));
    } else {
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
    field("Linked Radio VFO");
    if (ImGui::BeginCombo("##Linked Radio VFO", linked_vfo.empty()
                                                    ? "Choose channel"
                                                    : linked_vfo.c_str())) {
      for (auto &entry : gui::waterfall.vfos) {
        if (ImGui::Selectable(entry.first.c_str(), entry.first == linked_vfo)) {
          linked_vfo = entry.first;
          ui_initialized = false;
        }
      }
      ImGui::EndCombo();
    }
    field("Receive frequency (Hz)");
    draft_dirty |= ImGui::InputDouble("##Receive frequency", &dial_draft, 100,
                                      1000, "%.0f");
    if (ImGui::Button("Tune")) {
      if (std::isfinite(dial_draft) && dial_draft >= 70000 &&
          dial_draft <= 130000000)
        set(cmx::SetFrequency,
            cmx::integer(uint64_t(std::llround(dial_draft)), 8));
      draft_dirty = false;
    }
    field("Channel offset (Hz)");
    draft_dirty |= ImGui::InputInt("##Channel offset", &offset_draft);
    if (ImGui::Button("Apply offset")) {
      set(cmx::SetOffset, cmx::integer(uint32_t(offset_draft), 4));
      draft_dirty = false;
    }
    ImGui::Text("Spectrum center: %.6f MHz", s.center / 1e6);
    ImGui::TextWrapped(
        "Offset edits keep the CAT dial. Ordinary tuning keeps the offset.");
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
    if (!capacitor_dirty)
      capacitor_draft = s.lf_mf_capacitor;
    field("LF/MF capacitor (0–4095)");
    capacitor_dirty |= ImGui::InputInt("##LF/MF capacitor", &capacitor_draft);
    if (ImGui::Button("Apply capacitor") && capacitor_draft >= 0 &&
        capacitor_draft <= 4095) {
      set(cmx::SetLfMfCapacitor, cmx::integer(capacitor_draft, 2));
      capacitor_dirty = false;
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
      s.linked_vfo = "Astra test";
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
        ImGui::GetIO().MouseDown[ImGuiMouseButton_Left] = true;
        tuner::normalTuning("Astra test", double(before + 2000));
        s.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        s.tick();
        if (s.snapshot().state.requested != before)
          throw std::runtime_error("VFO drag retuned before mouse release");
        ImGui::GetIO().MouseDown[ImGuiMouseButton_Left] = false;
        s.tick();
        wait([&] { return s.snapshot().state.requested == before + 2000; });
        s.tick();
        if (sigpath::vfoManager.getOffset("Astra test") != 10000)
          throw std::runtime_error("Normal tune changed fixed offset");
        if (cmx::simulator_cat(endpoint, "FA;") !=
            "FA" + std::string(11 - std::to_string(before + 2000).size(), '0') +
                std::to_string(before + 2000) + ";")
          throw std::runtime_error("SDR tune did not propagate to CAT");
        const auto dial = s.snapshot().state.requested;
        gui::waterfall.selectedVFO = "Other test";
        tuner::normalTuning("Other test", double(dial + 15000));
        s.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        s.tick();
        if (s.snapshot().state.requested != dial)
          throw std::runtime_error("Secondary VFO changed CAT");
        tuner::normalTuning("Other test", double(dial + 500000));
        s.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        s.tick();
        if (s.snapshot().state.requested != dial ||
            gui::waterfall.getCenterFrequency() !=
                double(s.snapshot().state.center))
          throw std::runtime_error("Secondary VFO recentered the receiver");
        gui::waterfall.selectedVFO = "Astra test";
        if (cmx::simulator_cat(endpoint, "FA00007074049;FA;") !=
            "FA00007074049;")
          throw std::runtime_error("CAT retune failed");
        wait([&] { return s.snapshot().state.requested == 7074049; });
        s.tick();
        if (gui::waterfall.getCenterFrequency() != 7064049 ||
            sigpath::vfoManager.getOffset("Astra test") != 10000)
          throw std::runtime_error("CAT did not propagate to spectrum/VFO");
        auto n = count.load();
        wait([&] { return count > n + 12000; });
        s.stop();
        wait([&] { return !s.snapshot().playing; });
        if (!s.snapshot().state.configured)
          throw std::runtime_error("Stop stopped audio capture");
        s.set(cmx::SetOffset, cmx::integer(uint32_t(-10000), 4));
        wait([&] { return s.snapshot().state.offset == -10000; });
        if (s.snapshot().state.requested != 7074049)
          throw std::runtime_error("Offset edit moved CAT dial");
        s.start();
        n = count.load();
        wait([&] { return count > n + 12000; });
        s.stop();
        wait([&] { return !s.snapshot().playing; });
        s.enqueue({Request::Disconnect, 0, {}, {}});
        wait([&] { return !s.snapshot().connected; });
        s.connect();
        wait([&] { return s.snapshot().connected; });
        if (s.snapshot().state.requested != 7074049)
          throw std::runtime_error("Reconnect overwrote receiver state");
        flog::info("Astra module: linked VFO, independent secondary, CAT "
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
      linked_vfo = p.value("vfo", std::string());
    }
    config.release();
    if (const char *sim = std::getenv("ASTRA918_SIMULATOR")) {
      simulator = true;
      std::snprintf(address, sizeof(address), "%s", sim);
    }
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
    // The frame hook reads the final absolute linked VFO frequency; a source
    // callback alone cannot distinguish normal SDR++ VFO motion from LO motion.
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
    config.conf[name] = {{"serial", serial}, {"vfo", linked_vfo}};
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
