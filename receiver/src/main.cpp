// imacdisplayd: the iMac side of PROTOCOL.md.
// Done so far: session (HELLO, CONFIG, PING, BYE, 3 s timeout), keyboard and mouse forwarding, USB drives over SMB.
// Not yet: waiting screen, video decode and display, sound.
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "input.h"
#include "storage.h"
#include "net.h"
#include "protocol.h"

using json = nlohmann::json;

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

// What this iMac decodes at 60 fps, measured in docs/hardware-validation.md (A4): CPU only.
json imac_hello() {
  return {{"proto", proto::kVersion},
          {"role", "imac"},
          {"panel", {{"w", 5120}, {"h", 2880}, {"hz", 60}}},
          {"decode", {{"codecs", {"h264"}}, {"max_w", 3840}, {"max_h", 2160}, {"max_tiles", 4}, {"hw", false}}},
          {"audio", {{"out", false}, {"in", false}}},
          {"features", {"input", "storage"}}};
}

struct Options {
  std::string iface = "thunderbolt0";
  uint16_t port = proto::kPort;
  bool storage_read_only = false;
};

class Daemon {
 public:
  explicit Daemon(Options o)
      : opt_(std::move(o)),
        input_([this](uint8_t t, const void* p, size_t n) { send_ctrl(t, p, n); }),
        storage_({opt_.iface, opt_.storage_read_only},
                 [this](uint8_t t, const std::string& j) { send_ctrl(t, j.data(), j.size()); }) {
    timer_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    itimerspec its{{1, 0}, {1, 0}};
    timerfd_settime(timer_, 0, &its, nullptr);
  }

  int run() {
    fprintf(stderr, "imacdisplayd: listening on %s port %u\n", opt_.iface.c_str(), opt_.port);
    try_listen();
    while (!g_stop) {
      std::vector<pollfd> pfds;
      std::vector<int> kind;  // 0 timer 1 listen 2 pending 3 ctrl 4 video 5 inotify 6 input
      auto add = [&](int fd, short ev, int k) { pfds.push_back({fd, ev, 0}); kind.push_back(k); };
      add(timer_, POLLIN, 0);
      if (listen_fd_ >= 0) add(listen_fd_, POLLIN, 1);
      for (auto& c : pending_) add(c->fd(), POLLIN | (c->wants_write() ? POLLOUT : 0), 2);
      if (ctrl_) add(ctrl_->fd(), POLLIN | (ctrl_->wants_write() ? POLLOUT : 0), 3);
      if (video_) add(video_->fd(), POLLIN | (video_->wants_write() ? POLLOUT : 0), 4);
      add(input_.inotify_fd(), POLLIN, 5);
      add(storage_.udev_fd(), POLLIN, 7);
      for (int fd : input_.fds()) add(fd, POLLIN, 6);

      if (poll(pfds.data(), pfds.size(), -1) < 0) {
        if (errno == EINTR) continue;
        perror("poll");
        return 1;
      }
      for (size_t i = 0; i < pfds.size(); i++) {
        if (!pfds[i].revents) continue;
        int fd = pfds[i].fd;
        switch (kind[i]) {
          case 0: { uint64_t n; (void)!read(timer_, &n, sizeof n); tick(); break; }
          case 1: accept_all(); break;
          case 2: service_pending(fd, pfds[i].revents); break;
          case 3: if (ctrl_ && ctrl_->fd() == fd) service_ctrl(pfds[i].revents); break;
          case 4: if (video_ && video_->fd() == fd) service_video(pfds[i].revents); break;
          case 5: input_.on_inotify(); break;
          case 6: input_.on_readable(fd); break;
          case 7: storage_.on_udev(); break;
        }
      }
    }
    end_session("receiver shutting down");
    return 0;
  }

 private:
  void try_listen() {
    if (listen_fd_ >= 0) return;
    try {
      listen_fd_ = listen_on(opt_.iface, opt_.port);
      fprintf(stderr, "net: %s is up, accepting connections\n", opt_.iface.c_str());
      listen_err_.clear();
    } catch (const std::exception& e) {
      if (listen_err_ != e.what()) fprintf(stderr, "net: not listening yet (%s), retrying every second\n", e.what());
      listen_err_ = e.what();
    }
  }

  void accept_all() {
    for (;;) {
      int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        // The interface went away (cable unplugged): listen again once it is back.
        fprintf(stderr, "net: accept: %s\n", strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        return;
      }
      pending_.push_back(std::make_unique<Conn>(fd));
      fprintf(stderr, "net: connection from %s\n", pending_.back()->peer().c_str());
    }
  }

  void service_pending(int fd, short rev) {
    size_t idx = 0;
    while (idx < pending_.size() && pending_[idx]->fd() != fd) idx++;
    if (idx == pending_.size()) return;
    Conn* c = pending_[idx].get();
    if (rev & POLLOUT) c->on_writable();
    json hello;
    bool got = false, bad = false;
    bool ok = c->on_readable([&](const proto::Header& h, const uint8_t* p) {
      if (h.type != proto::HELLO) { bad = true; return false; }
      hello = json::parse(p, p + h.length, nullptr, false);
      got = true;
      return false;  // frames after HELLO stay buffered and are handled once the channel is known
    });
    if (!ok || bad || (got && hello.is_discarded())) {
      if (bad) bye(*c, "first frame must be HELLO");
      pending_.erase(pending_.begin() + idx);
      return;
    }
    if (!got) return;
    std::unique_ptr<Conn> conn = std::move(pending_[idx]);
    pending_.erase(pending_.begin() + idx);

    if (hello.value("proto", -1) != proto::kVersion || hello.value("role", "") != "mac") {
      bye(*conn, "protocol mismatch: this iMac speaks proto " + std::to_string(proto::kVersion));
      return;
    }
    std::string channel = hello.value("channel", "");
    std::string session = hello.value("session", "");
    if (channel == "ctrl") {
      if (ctrl_) end_session("replaced by a new session");
      ctrl_ = std::move(conn);
      session_ = session;
      fprintf(stderr, "session: ctrl from %s, \"%s\", session %s\n", ctrl_->peer().c_str(),
              hello.value("name", "?").c_str(), session_.c_str());
      ctrl_->send_json(proto::HELLO, imac_hello().dump());
      input_.set_active(true);
      storage_.session_started();
      service_ctrl(0, /*buffered_only=*/true);
    } else if (channel == "video" && ctrl_ && session == session_) {
      video_ = std::move(conn);
      video_->send_json(proto::HELLO, json{{"proto", proto::kVersion}, {"role", "imac"}}.dump());
      fprintf(stderr, "session: video connected\n");
      service_video(0, /*buffered_only=*/true);
    } else {
      bye(*conn, channel == "video" ? "no ctrl connection for this session" : "unknown channel");
    }
  }

  void service_ctrl(short rev, bool buffered_only = false) {
    if (rev & POLLOUT && !ctrl_->on_writable()) { end_session("ctrl write failed"); return; }
    bool ended = false;
    auto on_frame = [&](const proto::Header& h, const uint8_t* p) {
      switch (h.type) {
        case proto::PING: ctrl_->send(proto::PONG, p, h.length); break;
        case proto::PONG: break;
        case proto::CONFIG: handle_config(json::parse(p, p + h.length, nullptr, false)); break;
        case proto::STORAGE_EJECT: {
          json j = json::parse(p, p + h.length, nullptr, false);
          if (j.is_object()) storage_.eject(j.value("id", ""));
          break;
        }
        case proto::BYE: {
          json j = json::parse(p, p + h.length, nullptr, false);
          fprintf(stderr, "session: BYE from MacBook (%s)\n", j.is_object() ? j.value("reason", "").c_str() : "");
          ended = true;
          break;
        }
        default:
          if (!warned_[h.type]) fprintf(stderr, "ctrl: ignoring message type 0x%02x (not implemented yet)\n", h.type);
          warned_[h.type] = true;
      }
      return !ended;
    };
    bool ok = buffered_only ? ctrl_->process(on_frame) : ctrl_->on_readable(on_frame);
    if (ended) end_session(nullptr);
    else if (!ok) end_session("ctrl connection closed");
  }

  void handle_config(const json& c) {
    std::string err;
    if (!c.is_object() || !c.contains("video")) err = "CONFIG without video";
    else {
      const json& v = c["video"];
      if (v.value("codec", "") != "h264") err = "only h264";
      const json tiles = v.value("tiles", json::array());
      if (tiles.empty() || tiles.size() > 4) err = "1 to 4 tiles";
      for (const auto& t : tiles)
        if (t.value("w", 0) > 3840 || t.value("h", 0) > 2160) err = "tile larger than 3840x2160";
    }
    if (err.empty()) {
      config_ = c;
      fprintf(stderr, "session: CONFIG accepted: %s\n", c["video"].dump().c_str());
      ctrl_->send_json(proto::CONFIG_ACK, R"({"ok":true})");
    } else {
      fprintf(stderr, "session: CONFIG rejected: %s\n", err.c_str());
      ctrl_->send_json(proto::CONFIG_ACK, json{{"ok", false}, {"error", err}}.dump());
    }
  }

  void service_video(short rev, bool buffered_only = false) {
    if (rev & POLLOUT) video_->on_writable();
    auto on_frame = [&](const proto::Header& h, const uint8_t* p) {
      if (h.type == proto::VIDEO_FRAME) { video_frames_++; video_bytes_ += h.length; }
      return true;
    };
    bool ok = buffered_only ? video_->process(on_frame) : video_->on_readable(on_frame);
    if (!ok) {
      fprintf(stderr, "session: video connection closed\n");
      video_.reset();
    }
  }

  void tick() {
    try_listen();
    uint64_t now = now_us();
    // Connections that never sent HELLO.
    for (size_t i = 0; i < pending_.size();)
      if (now - pending_[i]->last_rx_us() > 3000000) pending_.erase(pending_.begin() + i);
      else i++;
    if (!ctrl_) return;
    if (now - ctrl_->last_rx_us() > 3000000) { end_session("nothing received on ctrl for 3 s"); return; }
    uint8_t ping[8];
    proto::put_u64(ping, now);
    ctrl_->send(proto::PING, ping, sizeof ping);
    if (video_frames_) {
      // Video is received but not decoded yet (stage 3 of the receiver).
      fprintf(stderr, "video: %llu frames, %.1f Mbit/s received (not decoded yet)\n",
              (unsigned long long)video_frames_, video_bytes_ * 8 / 1e6);
      video_frames_ = video_bytes_ = 0;
    }
  }

  void bye(Conn& c, const std::string& reason) {
    c.send_json(proto::BYE, json{{"reason", reason}}.dump());
    fprintf(stderr, "net: BYE to %s: %s\n", c.peer().c_str(), reason.c_str());
  }

  void end_session(const char* reason) {
    if (!ctrl_) return;
    if (reason) bye(*ctrl_, reason);
    ctrl_.reset();
    video_.reset();
    session_.clear();
    input_.set_active(false);
    storage_.session_ended();
    fprintf(stderr, "session: ended\n");
  }

  void send_ctrl(uint8_t type, const void* p, size_t n) {
    if (ctrl_) ctrl_->send(type, p, n);
  }

  Options opt_;
  Input input_;
  Storage storage_;
  int timer_ = -1;
  int listen_fd_ = -1;
  std::string listen_err_;
  std::vector<std::unique_ptr<Conn>> pending_;
  std::unique_ptr<Conn> ctrl_, video_;
  std::string session_;
  json config_;
  bool warned_[256] = {};
  uint64_t video_frames_ = 0, video_bytes_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--iface" && i + 1 < argc) o.iface = argv[++i];
    else if (a == "--port" && i + 1 < argc) o.port = std::stoi(argv[++i]);
    else if (a == "--storage-read-only") o.storage_read_only = true;
    else {
      fprintf(stderr,
              "usage: imacdisplayd [--iface IFACE] [--port PORT]\n"
              "  --iface  network interface to accept connections on (default thunderbolt0;\n"
              "           anything else is for testing only)\n"
              "  --storage-read-only  mount USB drives read-only (testing)\n");
      return 2;
    }
  }
  signal(SIGPIPE, SIG_IGN);
  struct sigaction sa {};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  setvbuf(stderr, nullptr, _IOLBF, 0);
  return Daemon(o).run();
}
