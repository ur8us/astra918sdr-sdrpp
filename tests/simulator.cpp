#include "astra_stream.h"
#include "cmx918_device.h"
#include <chrono>
#include <iostream>
#include <thread>
// Reproduce a USB FIFO containing the tail of an interrupted old frame. The
// same client lifecycle must drain this before accepting a new generation.
class StaleIqTransport final : public cmx::Transport {
  std::unique_ptr<cmx::Transport> inner;
  bool stale = true;
  std::chrono::steady_clock::time_point reply_ready{};

public:
  bool delay_next_status = false;
  explicit StaleIqTransport(std::unique_ptr<cmx::Transport> t)
      : inner(std::move(t)) {}
  void write_control(const cmx::Bytes &b) override {
    inner->write_control(b);
    if (b[5] == cmx::Status && delay_next_status) {
      delay_next_status = false;
      reply_ready = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    }
  }
  cmx::Bytes read_control(size_t n, unsigned timeout) override {
    if (std::chrono::steady_clock::now() < reply_ready) {
      std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
      return {};
    }
    return inner->read_control(n, timeout);
  }
  cmx::Bytes read_iq(size_t n, unsigned timeout) override {
    if (stale) {
      stale = false;
      return cmx::Bytes(64, 0x55);
    }
    return inner->read_iq(n, timeout);
  }
};
int main(int argc, char **argv) {
  try {
    auto transport = std::make_unique<StaleIqTransport>(
        cmx::open_tcp(argc > 1 ? argv[1] : "127.0.0.1:7350"));
    auto *timing = transport.get();
    cmx::Device d(std::move(transport));
    d.frequency(14074049);
    d.state =
        cmx::State::decode(d.command(cmx::SetOffset, cmx::integer(10000, 4)));
    if (d.state.requested != 14074049 || d.state.center != 14064049)
      throw std::runtime_error("Tuning invariant failed");
    d.start();
    cmx::AstraDecoder decoder;
    size_t samples = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (samples < 120000 && std::chrono::steady_clock::now() < deadline) {
      for (auto &b : decoder.feed(d.read_iq())) {
        if (b.center != 14064049)
          throw std::runtime_error("Wrong spectrum metadata");
        samples += b.samples.size();
      }
    }
    d.stop();
    // Firmware may service STATUS only after its eight-second RX recovery.
    // A delayed valid reply must retain control synchronization.
    timing->delay_next_status = true;
    d.refresh();
    if (samples < 120000 || !d.state.configured || d.state.streaming)
      throw std::runtime_error("Independent stream lifecycle failed");
    std::cout << "Portable C++ simulator test passed: " << samples
              << " samples\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
}
