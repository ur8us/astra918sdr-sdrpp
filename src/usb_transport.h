#pragma once
#include "cmx918_protocol.h"
#include <memory>
namespace cmx {
// Firmware RF reconfiguration can take eight seconds, plus relay/reset delays.
constexpr unsigned control_timeout_ms = 15000;
struct Transport {
  virtual ~Transport() = default;
  virtual void write_control(const Bytes &) = 0;
  virtual Bytes read_control(size_t maximum, unsigned timeout_ms) = 0;
  virtual Bytes read_iq(size_t maximum, unsigned timeout_ms) = 0;
};
struct DeviceInfo {
  std::string serial, label;
  bool present = true;
};
std::vector<DeviceInfo> enumerate_usb();
std::unique_ptr<Transport> open_usb(const std::string &serial);
std::unique_ptr<Transport> open_tcp(const std::string &address);
std::string simulator_cat(const std::string &address,
                          const std::string &command);
} // namespace cmx
