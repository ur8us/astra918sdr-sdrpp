#ifdef NDEBUG
#undef NDEBUG
#endif
#include "astra_stream.h"
#include <cassert>
#include <iostream>
int main() {
  auto record =
      cmx::record(cmx::SetOffset, 42, cmx::integer(uint32_t(-10000), 4));
  assert(std::equal(record.begin(), record.begin() + 4, "AST1"));
  assert(cmx::response(record, cmx::SetOffset, 42) ==
         cmx::integer(uint32_t(-10000), 4));
  auto bad = record;
  bad[255] = 1;
  bool rejected = false;
  try {
    cmx::response(bad, cmx::SetOffset, 42);
  } catch (...) {
    rejected = true;
  }
  assert(rejected);
  cmx::State state;
  state.offset = -10000;
  state.center = state.requested + 10000;
  auto parsed = cmx::State::decode(state.encode());
  assert(parsed.offset == -10000 && parsed.center == state.center);
  cmx::Bytes frame(2112);
  std::copy_n("ASIQ", 4, frame.begin());
  frame[4] = 1;
  frame[5] = 0x80;
  cmx::put(frame, 6, 64, 2);
  cmx::put(frame, 8, 1, 4);
  cmx::put(frame, 24, 120000, 4);
  cmx::put(frame, 28, 512, 2);
  cmx::put(frame, 32, 14200000, 8);
  cmx::put(frame, 40, 14200000, 8);
  cmx::put(frame, 64, 0x8000, 2);
  cmx::put(frame, 66, 32767, 2);
  cmx::AstraDecoder decoder;
  size_t count = 0;
  for (size_t p = 0; p < frame.size(); p += 37) {
    auto blocks = decoder.feed(cmx::Bytes(
        frame.begin() + p, frame.begin() + std::min(p + 37, frame.size())));
    for (auto &b : blocks) {
      count += b.samples.size();
      assert(b.samples[0].real() == -1.f);
      assert(b.samples[0].imag() > 0.99f);
    }
  }
  assert(count == 512);
  cmx::put(frame, 8, 2, 4);
  auto blocks = decoder.feed(frame);
  assert(blocks.size() == 1 && blocks[0].generation == 2);
  rejected = false;
  try {
    decoder.feed(frame);
  } catch (...) {
    rejected = true;
  }
  assert(rejected);
  std::cout << "Astra protocol, signed samples, fragmentation and external "
               "retune passed\n";
}
