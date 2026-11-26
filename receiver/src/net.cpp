#include "net.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

uint64_t now_us() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000000u + ts.tv_nsec / 1000;
}

static void set_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

int listen_on(const std::string& iface, uint16_t port) {
  int fd = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) throw std::runtime_error(std::string("socket: ") + strerror(errno));
  int one = 1, zero = 0;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
  // Only accept connections that arrive on this interface. Fails with ENODEV while the interface does not
  // exist (cable not plugged in); the caller retries.
  if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface.c_str(), iface.size()) < 0) {
    int e = errno; close(fd);
    throw std::runtime_error("SO_BINDTODEVICE " + iface + ": " + strerror(e));
  }
  sockaddr_in6 a{};
  a.sin6_family = AF_INET6;
  a.sin6_addr = in6addr_any;
  a.sin6_port = htons(port);
  if (bind(fd, (sockaddr*)&a, sizeof a) < 0 || listen(fd, 4) < 0) {
    int e = errno; close(fd);
    throw std::runtime_error("bind/listen port " + std::to_string(port) + ": " + strerror(e));
  }
  set_nonblock(fd);
  return fd;
}

Conn::Conn(int fd) : fd_(fd), last_rx_us_(now_us()) {
  set_nonblock(fd_);
  int one = 1;
  setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  sockaddr_storage ss{};
  socklen_t sl = sizeof ss;
  char host[INET6_ADDRSTRLEN + IF_NAMESIZE + 2] = "?";
  if (getpeername(fd_, (sockaddr*)&ss, &sl) == 0 && ss.ss_family == AF_INET6) {
    auto* s6 = (sockaddr_in6*)&ss;
    if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr)) inet_ntop(AF_INET, &s6->sin6_addr.s6_addr[12], host, sizeof host);
    else inet_ntop(AF_INET6, &s6->sin6_addr, host, sizeof host);
    if (s6->sin6_scope_id) {
      char ifn[IF_NAMESIZE];
      if (if_indextoname(s6->sin6_scope_id, ifn)) { strcat(host, "%"); strcat(host, ifn); }
    }
  }
  peer_ = host;
}

Conn::~Conn() { close(fd_); }

bool Conn::on_readable(const FrameFn& on_frame) {
  uint8_t buf[256 * 1024];
  for (;;) {
    ssize_t n = read(fd_, buf, sizeof buf);
    if (n > 0) {
      in_.insert(in_.end(), buf, buf + n);
      last_rx_us_ = now_us();
      continue;
    }
    if (n == 0) broken_ = true;
    else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) broken_ = true;
    break;
  }
  return process(on_frame);
}

bool Conn::process(const FrameFn& on_frame) {
  size_t off = 0;
  while (in_.size() - off >= proto::kHeaderSize) {
    proto::Header h = proto::decode_header(in_.data() + off);
    if (h.length > proto::kMaxPayload) { broken_ = true; break; }
    if (in_.size() - off < proto::kHeaderSize + h.length) break;
    bool more = on_frame(h, in_.data() + off + proto::kHeaderSize);
    off += proto::kHeaderSize + h.length;
    if (!more) break;
  }
  if (off) in_.erase(in_.begin(), in_.begin() + off);
  return !broken_;
}

bool Conn::send(uint8_t type, const void* payload, size_t len, uint8_t flags, uint16_t tile) {
  if (broken_) return false;
  std::vector<uint8_t> f(proto::kHeaderSize + len);
  proto::Header h;
  h.length = len; h.type = type; h.flags = flags; h.tile = tile; h.pts_us = now_us();
  proto::encode_header(f.data(), h);
  if (len) memcpy(f.data() + proto::kHeaderSize, payload, len);
  out_.push_back(std::move(f));
  return on_writable();
}

bool Conn::on_writable() {
  while (!out_.empty()) {
    auto& f = out_.front();
    ssize_t n = ::send(fd_, f.data() + out_off_, f.size() - out_off_, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
      broken_ = true;
      return false;
    }
    out_off_ += n;
    if (out_off_ == f.size()) { out_.pop_front(); out_off_ = 0; }
  }
  return true;
}
