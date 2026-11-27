// imacdisplayd: the iMac side of PROTOCOL.md.
// Done so far: session (HELLO, CONFIG, PING, BYE, 3 s timeout), keyboard and mouse forwarding, USB drives over SMB.
// Waiting screen on the bare panel, video (CPU decode per tile, GPU display), sound (ALSA).
#include <dirent.h>
#include <spawn.h>
#include <sys/stat.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "audio.h"
#include "display.h"
#include "input.h"
#include "storage.h"
#include "video.h"
#include "wifi.h"
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
          {"audio", {{"out", true}, {"in", true}}},
          {"features", {"input", "storage", "key-repeat"}}};
}

// The link chosen on the waiting screen (Thunderbolt or Wi-Fi), kept across restarts.
const char* kLinkFile = "/var/lib/imacdisplayd/link";

// The first Wi-Fi interface (one with a wireless/ directory in sysfs), or wlan0.
std::string wifi_iface() {
  if (DIR* d = opendir("/sys/class/net")) {
    while (dirent* e = readdir(d)) {
      if (e->d_name[0] == '.') continue;
      std::string w = std::string("/sys/class/net/") + e->d_name + "/wireless";
      if (access(w.c_str(), F_OK) == 0) { std::string n = e->d_name; closedir(d); return n; }
    }
    closedir(d);
  }
  return "wlan0";
}

bool is_wireless(const std::string& iface) {
  return access(("/sys/class/net/" + iface + "/wireless").c_str(), F_OK) == 0 || iface.rfind("wl", 0) == 0;
}

struct Options {
  std::string iface = "thunderbolt0";
  bool iface_given = false;  // --iface on the command line: fixed, the link file is not used
  uint16_t port = proto::kPort;
  bool storage_read_only = false;
  bool share_macos = true;
  bool display = true;
  std::string screenshot;  // write the waiting screen to this PNG once it is drawn (testing)
  std::string snapshots;   // directory: every redraw of the waiting screen as NNN.png (testing)
};

class Daemon {
 public:
  explicit Daemon(Options o)
      : opt_(std::move(o)),
        input_([this](uint8_t t, const void* p, size_t n) { send_ctrl(t, p, n); },
               [this](int dx, int dy, uint16_t code, int value) { local_input(dx, dy, code, value); }),
        storage_({opt_.iface, opt_.storage_read_only, opt_.share_macos},
                 [this](uint8_t t, const std::string& j) { send_ctrl(t, j.data(), j.size()); }) {
    timer_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    itimerspec its{{1, 0}, {1, 0}};
    timerfd_settime(timer_, 0, &its, nullptr);
    kf_efd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    kf_timer_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    mic_efd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    storage_.set_iface(opt_.iface, is_wireless(opt_.iface));
    if (opt_.display) wifi_ = std::make_unique<Wifi>(wifi_iface(), [this] { wifi_changed(); });
    anim_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    itimerspec ats{{0, 33333333}, {0, 33333333}};  // 30 fps: the waiting screen's background and dots
    timerfd_settime(anim_, 0, &ats, nullptr);
  }

  int run() {
    fprintf(stderr, "imacdisplayd: listening on %s port %u\n", opt_.iface.c_str(), opt_.port);
    try_listen();
    display_.set_snapshot_dir(opt_.snapshots);
    update_screen();
    while (!g_stop) {
      std::vector<pollfd> pfds;
      std::vector<int> kind;  // 0 timer 1 listen 2 pending 3 ctrl 4 video 5 inotify 6 input
      auto add = [&](int fd, short ev, int k) { pfds.push_back({fd, ev, 0}); kind.push_back(k); };
      add(timer_, POLLIN, 0);
      if (display_.is_open()) add(anim_, POLLIN, 8);
      if (listen_fd_ >= 0) add(listen_fd_, POLLIN, 1);
      for (auto& c : pending_) add(c->fd(), POLLIN | (c->wants_write() ? POLLOUT : 0), 2);
      if (ctrl_) add(ctrl_->fd(), POLLIN | (ctrl_->wants_write() ? POLLOUT : 0), 3);
      if (video_) add(video_->fd(), POLLIN | (video_->wants_write() ? POLLOUT : 0), 4);
      add(input_.notify_fd(), POLLIN, 5);
      add(storage_.udev_fd(), POLLIN, 7);
      add(kf_efd_, POLLIN, 9);
      add(kf_timer_, POLLIN, 10);
      add(mic_efd_, POLLIN, 11);
      if (wifi_ && wifi_->ok()) add(wifi_->fd(), wifi_->events(), 12);
      if (display_.flip_fd() >= 0) add(display_.flip_fd(), POLLIN, 13);

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
          case 5: input_.drain(); break;
          case 7: storage_.on_udev(); break;
          case 8: {
            uint64_t n;
            (void)!read(anim_, &n, sizeof n);
            video_took_screen();
            display_.animate();
            break;
          }
          case 9: { uint64_t n; (void)!read(kf_efd_, &n, sizeof n); send_keyframe_request(); break; }
          case 11: { uint64_t n; (void)!read(mic_efd_, &n, sizeof n); send_mic(); break; }
          case 10: { uint64_t n; (void)!read(kf_timer_, &n, sizeof n); if (kf_pending_) send_keyframe_request(); break; }
          case 12: wifi_->process(); break;
          case 13: display_.on_drm_event(); break;
        }
      }
    }
    end_session("receiver shutting down");
    stop_video();
    display_.close();
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

  // --- the iMac's own mouse and keyboard on the waiting screen ---------------------------------
  void local_input(int dx, int dy, uint16_t code, int value) {
    if (ctrl_ || !opt_.display) return;  // in a session everything goes to the MacBook
    if (!code) { display_.pointer_motion(dx, dy); return; }
    switch (display_.press(code, value)) {
      case Display::Action::Restart: power("reboot", "Restarting\u2026"); break;
      case Display::Action::ShutDown: power("poweroff", "Shutting down\u2026"); break;
      case Display::Action::UseThunderbolt: switch_link(false); break;
      case Display::Action::UseWifi: switch_link(true); break;
      case Display::Action::WifiOpen:
        if (wifi_) { wifi_->scan(); wifi_changed(); }
        break;
      case Display::Action::WifiJoin:
        if (const WifiNet* n = wifi_net(display_.wifi_pick())) {
          // A secured network iwd does not know yet: ask for the password first.
          if (n->type == "psk" && !n->known) display_.ask_wifi_password(n->name, "");
          else wifi_->connect(n->path, "");
        }
        break;
      case Display::Action::WifiPassword:
        if (const WifiNet* n = wifi_net(display_.wifi_pick())) wifi_->connect(n->path, display_.take_wifi_password());
        break;
      case Display::Action::None: break;
    }
  }

  const WifiNet* wifi_net(const std::string& name) const {
    if (!wifi_) return nullptr;
    for (auto& n : wifi_->networks())
      if (n.name == name) return &n;
    return nullptr;
  }

  void wifi_changed() {
    if (!wifi_) return;
    std::vector<Display::WifiRow> rows;
    for (auto& n : wifi_->networks())
      rows.push_back({n.name, n.bars, n.type != "open", n.connected});
    display_.set_wifi(rows, wifi_->scanning(), wifi_->status());
    // iwd asked for a password we did not have (new network, or the saved one stopped working).
    std::string need = wifi_->password_needed();
    if (!need.empty() && display_.wifi_open())
      for (auto& n : wifi_->networks())
        if (n.path == need) {
          bool known = n.known;
          display_.ask_wifi_password(n.name, wifi_->status().empty() && known ? "The saved password did not work." : wifi_->status());
          break;
        }
  }

  // Restart (into macOS, the firmware's default) or shut down. Nothing about the boot order is changed.
  void power(const char* verb, const char* text) {
    fprintf(stderr, "power: %s requested on the waiting screen\n", verb);
    display_.show_busy(text);
    pid_t pid;
    const char* argv[] = {"systemctl", verb, nullptr};
    if (posix_spawnp(&pid, "systemctl", nullptr, nullptr, (char* const*)argv, environ) != 0)
      fprintf(stderr, "power: cannot run systemctl %s: %s\n", verb, strerror(errno));
  }

  // Thunderbolt (the product) or Wi-Fi (testing). Only between sessions; listens on the new link at once.
  void switch_link(bool wifi) {
    if (ctrl_) return;
    std::string iface = wifi ? wifi_iface() : "thunderbolt0";
    if (iface == opt_.iface) return;
    fprintf(stderr, "net: switching the link from %s to %s\n", opt_.iface.c_str(), iface.c_str());
    pending_.clear();
    if (listen_fd_ >= 0) close(listen_fd_);
    listen_fd_ = -1;
    listen_err_.clear();
    opt_.iface = iface;
    storage_.set_iface(iface, is_wireless(iface));
    mkdir("/var/lib/imacdisplayd", 0755);
    if (FILE* f = fopen(kLinkFile, "w")) { fputs(wifi ? "wifi\n" : "thunderbolt\n", f); fclose(f); }
    try_listen();
    update_screen();
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
      mac_name_ = hello.value("name", "");
      fprintf(stderr, "session: ctrl from %s, \"%s\", session %s\n", ctrl_->peer().c_str(),
              mac_name_.c_str(), session_.c_str());
      ctrl_->send_json(proto::HELLO, imac_hello().dump());
      input_.set_active(true);
      storage_.session_started();
      cpu_performance(true);
      update_screen();
      service_ctrl(0, /*buffered_only=*/true);
    } else if (channel == "video" && ctrl_ && session == session_) {
      video_ = std::move(conn);
      video_->send_json(proto::HELLO, json{{"proto", proto::kVersion}, {"role", "imac"}}.dump());
      fprintf(stderr, "session: video connected\n");
      if (!video_dec_ && config_.contains("video")) start_video(config_["video"]);
      update_screen();
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
        case proto::AUDIO_OUT:
          if (audio_) audio_->play(p, h.length);
          break;
        case proto::AUDIO_VOLUME: {
          json j = json::parse(p, p + h.length, nullptr, false);
          if (j.is_object()) Audio::set_volume(j.value("volume", 1.0), j.value("mute", false));
          break;
        }
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
      bool audio_changed = !audio_ || c.value("audio_out", json()) != config_.value("audio_out", json()) ||
                           c.value("audio_in", json()) != config_.value("audio_in", json());
      config_ = c;
      config_us_ = now_us();
      // The MacBook's key repeat settings; without them (an older app) it repeats keys itself.
      const json kr = c.value("key_repeat", json::object());
      input_.set_repeat(kr.value("delay_ms", 0), kr.value("interval_ms", 0));
      fprintf(stderr, "session: CONFIG accepted: %s\n", c["video"].dump().c_str());
      ctrl_->send_json(proto::CONFIG_ACK, R"({"ok":true})");
      // Video (decoders, GPU setup) starts here, so it is ready when the first picture arrives; it takes
      // the panel only with that first picture, so a sound-only session (no video connection) keeps the
      // waiting screen. A CONFIG in the middle of a session (resolution change on the Mac) restarts it.
      start_video(c["video"]);
      // Sound restarts only if its own settings changed: a resolution change leaves no gap in the sound.
      if (audio_changed) start_audio(c);
      update_screen();
    } else {
      fprintf(stderr, "session: CONFIG rejected: %s\n", err.c_str());
      ctrl_->send_json(proto::CONFIG_ACK, json{{"ok", false}, {"error", err}}.dump());
    }
  }

  void service_video(short rev, bool buffered_only = false) {
    if (rev & POLLOUT) video_->on_writable();
    auto on_frame = [&](const proto::Header& h, const uint8_t* p) {
      if (h.type == proto::VIDEO_FRAME && video_dec_) video_dec_->push(h.tile, h.flags, h.pts_us, p, h.length);
      return true;
    };
    bool ok = buffered_only ? video_->process(on_frame) : video_->on_readable(on_frame);
    if (!ok) {
      fprintf(stderr, "session: video connection closed\n");
      video_.reset();
      stop_video();
    }
  }

  void start_video(const json& v) {
    if (video_dec_) {
      // Resolution change: the old picture goes, the panel shows black (like a monitor switching modes)
      // until the new stream's first picture, not the waiting screen.
      blank_on_release_ = true;
      video_dec_.reset();
      blank_on_release_ = false;
      restart_deadline_us_ = now_us() + 3000000;  // waiting screen again if no picture comes
    }
    VideoConfig cfg;
    cfg.w = v.value("w", 0);
    cfg.h = v.value("h", 0);
    for (const auto& t : v["tiles"])
      cfg.tiles.push_back({t.value("id", 0), t.value("x", 0), t.value("y", 0), t.value("w", 0), t.value("h", 0)});
    ScreenTarget target;  // fd -1: decode without showing (no display)
    if (display_.is_open()) target = display_.target();
    // The waiting screen keeps animating until the first picture takes the panel (see video_took_screen).
    video_dec_ = std::make_unique<Video>(
        target, cfg,
        [this] { uint64_t one = 1; (void)!write(kf_efd_, &one, sizeof one); },
        [this] { if (blank_on_release_) display_.blank(); else display_.resume(); },
        [this] { display_.hand_over(); });
  }

  // Once the presenter has put its first picture up, the waiting screen stops drawing.
  void video_took_screen() {
    if (video_dec_ && video_dec_->showing() && !display_.paused()) display_.pause();
  }

  // Sound: playback per CONFIG.audio_out, microphone per CONFIG.audio_in. s16le is the only format.
  void start_audio(const json& c) {
    audio_.reset();
    AudioFormat out, in;
    json ao = c.value("audio_out", json::object()), ai = c.value("audio_in", json::object());
    out.rate = ao.value("rate", 48000);
    out.channels = ao.value("ch", 2);
    in.rate = ai.value("rate", 48000);
    in.channels = ai.value("ch", 1);
    bool in_on = ai.value("enabled", false);
    if (ao.value("fmt", "s16le") != "s16le" || (in_on && ai.value("fmt", "s16le") != "s16le")) {
      fprintf(stderr, "audio: only s16le is supported, sound off\n");
      return;
    }
    audio_ = std::make_unique<Audio>(out, in_on, in, [this](std::vector<uint8_t> pcm) {
      {
        std::lock_guard<std::mutex> l(mic_mu_);
        if (mic_q_.size() < 20) mic_q_.push_back(std::move(pcm));  // 200 ms at most if ctrl is stuck
      }
      uint64_t one = 1;
      (void)!write(mic_efd_, &one, sizeof one);
    });
    fprintf(stderr, "audio: playing %d Hz %d ch%s\n", out.rate, out.channels,
            in_on ? (", microphone " + std::to_string(in.rate) + " Hz " + std::to_string(in.channels) + " ch").c_str() : "");
  }

  void send_mic() {
    std::deque<std::vector<uint8_t>> q;
    {
      std::lock_guard<std::mutex> l(mic_mu_);
      q.swap(mic_q_);
    }
    if (!ctrl_ || !audio_) return;
    for (auto& pcm : q) ctrl_->send(proto::AUDIO_IN, pcm.data(), pcm.size());
  }

  void stop_video() {
    if (!video_dec_) return;
    blank_on_release_ = false;
    video_dec_.reset();   // the presenter puts the waiting screen back before freeing its buffers
    display_.resume();    // (also when no picture was ever shown)
    update_screen();
  }

  // At most one request per 250 ms. A request that comes too soon is not dropped: it is sent as soon as
  // the 250 ms are over (a decoder waiting for a keyframe shows nothing until one arrives).
  void send_keyframe_request() {
    uint64_t now = now_us(), gap = 250000;
    if (!ctrl_) { kf_pending_ = false; return; }
    if (now - last_kf_request_us_ < gap) {
      if (!kf_pending_) {
        kf_pending_ = true;
        uint64_t wait = gap - (now - last_kf_request_us_);
        itimerspec its{{0, 0}, {time_t(wait / 1000000), long(wait % 1000000) * 1000}};
        timerfd_settime(kf_timer_, 0, &its, nullptr);
      }
      return;
    }
    kf_pending_ = false;
    last_kf_request_us_ = now;
    ctrl_->send(proto::VIDEO_REQUEST_KEYFRAME, nullptr, 0);
    fprintf(stderr, "video: asked the MacBook for a keyframe\n");
  }

  // While a session runs, ask the CPU for speed over power saving (intel_pstate energy preference).
  // Decoding comes in short bursts; with the default preference the clock stays low and a 1080p frame
  // takes 12 ms instead of 4 (measured, docs/hardware-validation.md). Restored at the waiting screen.
  void cpu_performance(bool on) {
    if (on == cpu_perf_on_) return;
    cpu_perf_on_ = on;
    for (int cpu = 0;; cpu++) {
      char path[96];
      snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/energy_performance_preference", cpu);
      FILE* f = fopen(path, on ? "r+" : "w");
      if (!f) break;
      if (on) {
        char old[64] = "";
        if (fgets(old, sizeof old, f)) old[strcspn(old, "\n")] = 0;
        if (cpu == 0) saved_epp_ = old;
        rewind(f);
        fputs("performance", f);
      } else {
        fputs(saved_epp_.empty() ? "balance_performance" : saved_epp_.c_str(), f);
      }
      fclose(f);
    }
    fprintf(stderr, "cpu: energy preference %s\n", on ? "performance (session)" : saved_epp_.c_str());
  }

  // Waiting screen: cable state, session state, and the link's addresses in the footer.
  void update_screen() {
    if (!opt_.display) return;
    if (!display_.is_open() && !display_.open()) return;
    Display::Status st;
    if (ctrl_) {
      st.state = Display::State::Connected;
      st.mac_name = mac_name_;
      // A session without a video connection after a while: the Mac has "Use Display" off.
      if (!video_ && config_us_ && now_us() - config_us_ > 1500000) st.detail = "Sound only. The picture is off on your Mac.";
    }
    else if (listen_fd_ >= 0 && iface_up()) st.state = Display::State::WaitingForMac;
    else st.state = Display::State::NoCable;
    st.menu = !ctrl_;
    st.wifi = is_wireless(opt_.iface);
    st.footer = footer();
    display_.show(st);
    if (!opt_.screenshot.empty()) {
      fprintf(stderr, "display: screenshot %s: %s\n", opt_.screenshot.c_str(), display_.save_png(opt_.screenshot) ? "ok" : "failed");
      opt_.screenshot.clear();
    }
  }

  bool iface_up() const {
    unsigned idx = if_nametoindex(opt_.iface.c_str());
    if (!idx) return false;
    char path[128];
    snprintf(path, sizeof path, "/sys/class/net/%s/carrier", opt_.iface.c_str());
    FILE* f = fopen(path, "r");
    int c = 0;
    if (f) { if (fscanf(f, "%d", &c) != 1) c = 0; fclose(f); }
    return c == 1;
  }

  std::string footer() const {
    char host[64] = "";
    gethostname(host, sizeof host - 1);
    std::string s = std::string(host) + "  \u00b7  " + opt_.iface;
    std::string addrs;
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) == 0) {
      for (ifaddrs* a = ifa; a; a = a->ifa_next) {
        if (!a->ifa_addr || opt_.iface != a->ifa_name || a->ifa_addr->sa_family != AF_INET) continue;
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((sockaddr_in*)a->ifa_addr)->sin_addr, buf, sizeof buf);
        addrs += std::string("  ") + buf;
      }
      freeifaddrs(ifa);
    }
    return s + (addrs.empty() ? "  not connected" : addrs);
  }

  void tick() {
    try_listen();
    display_.idle_tick();
    if (wifi_) wifi_->process();
    video_took_screen();
    if (video_dec_ && !video_dec_->showing() && display_.paused() && restart_deadline_us_ &&
        now_us() > restart_deadline_us_) {
      restart_deadline_us_ = 0;  // black after a resolution change, but no picture came: waiting screen
      display_.resume();
    }
    update_screen();
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
    if (video_dec_) {
      VideoStats s = video_dec_->take_stats();
      char buf[160];
      snprintf(buf, sizeof buf, R"({"decoded":%u,"dropped":%u,"decode_ms":%.1f,"present_ms":%.1f})", s.decoded,
               s.dropped, s.decode_ms, s.present_ms);
      ctrl_->send_json(proto::VIDEO_STATS, buf);
      if (audio_ && stats_log_ % 5 == 4) {
        AudioStats a = audio_->take_stats();
        fprintf(stderr, "audio: %u packets, buffer %.1f ms, %u underruns, %u trimmed, %u mic packets sent\n",
                a.received, a.buffer_ms, a.underruns, a.trimmed, a.mic_sent);
      }
      if (++stats_log_ % 5 == 0 || s.dropped)
        fprintf(stderr,
                "video: %u decoded, %u shown, %u dropped, receive-to-screen %.1f ms (per tile: decode %.1f, "
                "copy %.1f; then wait %.1f, upload+draw %.1f, swap %.1f, flip %.1f)\n",
                s.decoded, s.shown, s.dropped, s.present_ms, s.decode_ms, s.copy_ms, s.wait_ms, s.upload_ms,
                s.swap_ms, s.flip_ms);
    }
  }

  void bye(Conn& c, const std::string& reason) {
    c.send_json(proto::BYE, json{{"reason", reason}}.dump());
    fprintf(stderr, "net: BYE to %s: %s\n", c.peer().c_str(), reason.c_str());
  }

  void end_session(const char* reason) {
    if (!ctrl_) return;
    if (reason) bye(*ctrl_, reason);
    stop_video();
    audio_.reset();
    config_ = json();
    config_us_ = 0;
    ctrl_.reset();
    video_.reset();
    session_.clear();
    input_.set_active(false);
    storage_.session_ended();
    cpu_performance(false);
    kf_pending_ = false;
    fprintf(stderr, "session: ended\n");
    update_screen();
  }

  void send_ctrl(uint8_t type, const void* p, size_t n) {
    if (ctrl_) ctrl_->send(type, p, n);
  }

  Options opt_;
  Display display_;
  int anim_ = -1;
  std::string mac_name_;
  Input input_;
  Storage storage_;
  std::unique_ptr<Wifi> wifi_;
  int timer_ = -1;
  int listen_fd_ = -1;
  std::string listen_err_;
  std::vector<std::unique_ptr<Conn>> pending_;
  std::unique_ptr<Conn> ctrl_, video_;
  std::string session_;
  json config_;
  bool warned_[256] = {};
  std::unique_ptr<Video> video_dec_;
  bool blank_on_release_ = false;
  uint64_t restart_deadline_us_ = 0, config_us_ = 0;
  std::unique_ptr<Audio> audio_;
  std::mutex mic_mu_;
  std::deque<std::vector<uint8_t>> mic_q_;
  int mic_efd_ = -1;
  int kf_efd_ = -1, kf_timer_ = -1;
  bool kf_pending_ = false;
  bool cpu_perf_on_ = false;
  std::string saved_epp_;
  uint64_t last_kf_request_us_ = 0;
  unsigned stats_log_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--iface" && i + 1 < argc) { o.iface = argv[++i]; o.iface_given = true; }
    else if (a == "--port" && i + 1 < argc) o.port = std::stoi(argv[++i]);
    else if (a == "--storage-read-only") o.storage_read_only = true;
    else if (a == "--no-macos-share") o.share_macos = false;
    else if (a == "--no-display") o.display = false;
    else if (a == "--screenshot" && i + 1 < argc) o.screenshot = argv[++i];
    else if (a == "--snapshots" && i + 1 < argc) o.snapshots = argv[++i];
    else if (a == "--render-splash" && i + 1 < argc) {  // write the boot splash theme and exit
      std::string dir = argv[++i];
      bool ok = Display().render_splash(dir, 3840, 2160);
      fprintf(stderr, "splash: %s %s\n", dir.c_str(), ok ? "written" : "failed");
      return ok ? 0 : 1;
    }
    else {
      fprintf(stderr,
              "usage: imacdisplayd [--iface IFACE] [--port PORT]\n"
              "  --iface  network interface to accept connections on (default: the link chosen on the\n"
              "           waiting screen, thunderbolt0 or Wi-Fi; given here, the switch has no lasting effect)\n"
              "  --storage-read-only  mount USB drives read-only (testing)\n"
              "  --no-macos-share     do not offer the iMac's own macOS Data volume (read-only by default)\n"
              "  --no-display         leave the screen alone (testing next to a running receiver)\n");
      return 2;
    }
  }
  if (!o.iface_given) {  // the link chosen on the waiting screen last time
    if (FILE* f = fopen(kLinkFile, "r")) {
      char buf[32] = "";
      if (fgets(buf, sizeof buf, f) && !strncmp(buf, "wifi", 4)) o.iface = wifi_iface();
      fclose(f);
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
