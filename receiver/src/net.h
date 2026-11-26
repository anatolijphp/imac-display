#pragma once
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "protocol.h"

uint64_t now_us();  // CLOCK_MONOTONIC in microseconds

// TCP listener bound to one network interface (SO_BINDTODEVICE), IPv6 dual-stack.
int listen_on(const std::string& iface, uint16_t port);

// One framed, non-blocking TCP connection.
class Conn {
 public:
  explicit Conn(int fd);
  ~Conn();
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;

  int fd() const { return fd_; }
  std::string peer() const { return peer_; }

  // on_frame returns false to stop; the remaining frames stay buffered for the next process() call.
  using FrameFn = std::function<bool(const proto::Header&, const uint8_t*)>;
  // Reads what is available, then process(). Returns false when the connection is closed or broken.
  bool on_readable(const FrameFn& on_frame);
  // Calls on_frame for every complete buffered frame.
  bool process(const FrameFn& on_frame);
  // Queues a frame; tries to write immediately. Returns false if the connection is broken.
  bool send(uint8_t type, const void* payload, size_t len, uint8_t flags = 0, uint16_t tile = 0);
  bool send_json(uint8_t type, const std::string& json) { return send(type, json.data(), json.size()); }
  bool on_writable();
  bool wants_write() const { return !out_.empty(); }
  uint64_t last_rx_us() const { return last_rx_us_; }

 private:
  int fd_;
  std::string peer_;
  std::vector<uint8_t> in_;
  std::deque<std::vector<uint8_t>> out_;
  size_t out_off_ = 0;
  uint64_t last_rx_us_;
  bool broken_ = false;
};
