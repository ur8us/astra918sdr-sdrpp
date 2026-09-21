#include "usb_transport.h"
#include <algorithm>
#include <climits>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket invalid = INVALID_SOCKET;
static void close_socket(Socket s) { closesocket(s); }
#else
#include <cerrno>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket invalid = -1;
static void close_socket(Socket s) { close(s); }
#endif
namespace cmx {
namespace {
struct Tcp final : Transport {
  Socket control = invalid, iq = invalid;
  static Socket connect_to(const std::string &host, int port) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    addrinfo *list = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &list))
      throw std::runtime_error("Simulator address lookup failed");
    Socket s = invalid;
    for (auto p = list; p; p = p->ai_next) {
      s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
      if (s == invalid)
        continue;
#ifdef SO_NOSIGPIPE
      int no_sigpipe = 1;
      setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
      if (::connect(s, p->ai_addr, int(p->ai_addrlen)) == 0)
        break;
      close_socket(s);
      s = invalid;
    }
    freeaddrinfo(list);
    if (s == invalid)
      throw std::runtime_error("Cannot connect to Astra simulator");
    return s;
  }
  static void timeout(Socket s, unsigned ms) {
#ifdef _WIN32
    DWORD value = ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&value), sizeof(value));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char *>(&value), sizeof(value));
#else
    timeval value{};
    value.tv_sec = ms / 1000;
    value.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
#endif
  }
  explicit Tcp(const std::string &address) {
#ifdef _WIN32
    static struct Wsa {
      Wsa() {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data))
          throw std::runtime_error("WSAStartup failed");
      }
      ~Wsa() { WSACleanup(); }
    } wsa;
#endif
    auto pos = address.rfind(':');
    if (pos == std::string::npos)
      throw std::runtime_error("Use HOST:PORT");
    auto host = address.substr(0, pos);
    int port = std::stoi(address.substr(pos + 1));
    if (port < 1 || port > 65534)
      throw std::runtime_error("Invalid simulator port");
    try {
      control = connect_to(host, port);
      timeout(control, 5000);
      iq = connect_to(host, port + 1);
    } catch (...) {
      if (control != invalid)
        close_socket(control);
      throw;
    }
  }
  ~Tcp() override {
    if (control != invalid)
      close_socket(control);
    if (iq != invalid)
      close_socket(iq);
  }
  static Bytes read(Socket s, size_t n, unsigned ms) {
    timeout(s, ms);
    Bytes bytes(n);
    int got = recv(s, reinterpret_cast<char *>(bytes.data()), int(n), 0);
    if (got < 0) {
#ifdef _WIN32
      int e = WSAGetLastError();
      if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK)
        return {};
#else
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return {};
#endif
      throw std::runtime_error("Simulator read failed");
    }
    if (got == 0)
      throw std::runtime_error("Simulator disconnected");
    bytes.resize(size_t(got));
    return bytes;
  }
  void write_control(const Bytes &b) override {
    size_t used = 0;
    while (used < b.size()) {
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      int n = send(control, reinterpret_cast<const char *>(b.data() + used),
                   int(b.size() - used), flags);
      if (n <= 0)
        throw std::runtime_error("Simulator write failed");
      used += size_t(n);
    }
  }
  Bytes read_control(size_t n, unsigned ms) override {
    return read(control, n, ms);
  }
  Bytes read_iq(size_t n, unsigned ms) override { return read(iq, n, ms); }
};
} // namespace
std::unique_ptr<Transport> open_tcp(const std::string &address) {
  return std::make_unique<Tcp>(address);
}
std::string simulator_cat(const std::string &address,
                          const std::string &command) {
  const auto pos = address.rfind(':');
  if (pos == std::string::npos)
    throw std::runtime_error("Use HOST:PORT");
  auto s = Tcp::connect_to(address.substr(0, pos),
                           std::stoi(address.substr(pos + 1)) + 2);
  Tcp::timeout(s, 2000);
  try {
    size_t used = 0;
    while (used < command.size()) {
      int n = send(s, command.data() + used, int(command.size() - used), 0);
      if (n <= 0)
        throw std::runtime_error("CAT simulator write failed");
      used += size_t(n);
    }
    std::string result;
    while (result.size() < 256) {
      auto bytes = Tcp::read(s, 64, 2000);
      if (bytes.empty())
        throw std::runtime_error("CAT timeout");
      result.append(bytes.begin(), bytes.end());
      if (result.back() == ';') {
        close_socket(s);
        return result;
      }
    }
    throw std::runtime_error("CAT oversized reply");
  } catch (...) {
    close_socket(s);
    throw;
  }
}
} // namespace cmx
