#include "astra_stream.h"
#include "cmx918_device.h"
#include <chrono>
#include <iostream>
int main(int argc, char **argv) {
  try {
    cmx::Device d(cmx::open_tcp(argc > 1 ? argv[1] : "127.0.0.1:7350"));
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
