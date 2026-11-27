#include "input.h"

#include <dirent.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "net.h"
#include "protocol.h"

static const char* kDir = "/dev/input";

Input::Input(Emit emit, Local local, bool test_devices)
    : out_emit_(std::move(emit)), out_local_(std::move(local)), test_devices_(test_devices) {
  out_efd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  wake_efd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  auto notify = [this] { uint64_t one = 1; (void)!write(out_efd_, &one, sizeof one); };
  emit_ = [this, notify](uint8_t t, const void* p, size_t n) {
    std::lock_guard<std::mutex> l(mu_);
    queue_.push_back(Msg{false, t, std::vector<uint8_t>((const uint8_t*)p, (const uint8_t*)p + n), 0, 0, 0, 0, now_us()});
    notify();
  };
  local_ = [this, notify](int dx, int dy, uint16_t code, int value) {
    std::lock_guard<std::mutex> l(mu_);
    // Each report on its own (not summed): the pointer's acceleration depends on the size of each.
    queue_.push_back(Msg{true, 0, {}, dx, dy, code, value, now_us()});
    notify();
  };
  inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  inotify_add_watch(inotify_fd_, kDir, IN_CREATE | IN_ATTRIB | IN_DELETE);
  if (DIR* d = opendir(kDir)) {
    while (dirent* e = readdir(d))
      if (!strncmp(e->d_name, "event", 5)) try_open(std::string(kDir) + "/" + e->d_name);
    closedir(d);
  }
  thread_ = std::thread(&Input::loop, this);
}

Input::~Input() {
  stop_ = true;
  uint64_t one = 1;
  (void)!write(wake_efd_, &one, sizeof one);
  thread_.join();
  while (!devs_.empty()) close_dev(devs_.begin()->first);
  if (inotify_fd_ >= 0) close(inotify_fd_);
  close(out_efd_);
  close(wake_efd_);
}

void Input::loop() {
  while (!stop_) {
    if (want_active_ != active_) apply_active(want_active_);
    std::vector<pollfd> p{{inotify_fd_, POLLIN, 0}, {wake_efd_, POLLIN, 0}};
    for (auto& [fd, d] : devs_) p.push_back({fd, POLLIN, 0});
    int timeout = 500;
    if (rep_fd_ >= 0) {
      uint64_t now = now_us();
      timeout = rep_next_us_ <= now ? 0 : int(std::min<uint64_t>((rep_next_us_ - now + 999) / 1000, 500));
    }
    if (poll(p.data(), p.size(), timeout) > 0) {
      if (p[1].revents) { uint64_t n; (void)!read(wake_efd_, &n, sizeof n); }
      if (p[0].revents) on_inotify();
      for (size_t i = 2; i < p.size(); i++)
        if (p[i].revents) on_readable(p[i].fd);
    }
    tick_repeat();  // after the events: a release that is already here stops the repeat first
  }
}

void Input::set_repeat(int delay_ms, int interval_ms) {
  bool on = delay_ms > 0 && interval_ms > 0;
  repeat_delay_ms_ = on ? delay_ms : 0;
  repeat_interval_ms_ = on ? interval_ms : 0;
}

void Input::tick_repeat() {
  if (rep_fd_ < 0) return;
  int interval = repeat_interval_ms_;
  if (!active_ || interval <= 0) { rep_fd_ = -1; return; }
  uint64_t now = now_us();
  if (now < rep_next_us_) return;
  send_key(rep_code_, 2);
  // Never a burst: if this thread was held up, the next repeat is still at least half an interval away.
  rep_next_us_ = std::max(rep_next_us_ + uint64_t(interval) * 1000, now + uint64_t(interval) * 500);
}

void Input::drain() {
  uint64_t n;
  (void)!read(out_efd_, &n, sizeof n);
  std::vector<Msg> q;
  {
    std::lock_guard<std::mutex> l(mu_);
    q.swap(queue_);
  }
  // Events wait here while the main loop is busy with something else. A long wait delays keys before
  // they are even sent, which the MacBook cannot see: say so (not which key).
  const uint64_t now = now_us();
  if (!q.empty() && !q.front().local && now - q.front().queued_us > 100000) {
    slow_drains_++;
    if (now - slow_drain_logged_us_ > 2000000) {
      slow_drain_logged_us_ = now;
      fprintf(stderr, "input: keyboard and mouse events waited %llu ms for the main loop (%u time(s) so far)\n",
              (unsigned long long)((now - q.front().queued_us) / 1000), slow_drains_);
    }
  }
  for (auto& m : q) {
    if (m.local) { if (out_local_) out_local_(m.dx, m.dy, m.code, m.value); }
    else if (out_emit_) out_emit_(m.type, m.payload.data(), m.payload.size());
  }
}

void Input::set_active(bool on) {
  want_active_ = on;
  uint64_t one = 1;
  (void)!write(wake_efd_, &one, sizeof one);
}

void Input::try_open(const std::string& path) {
  for (auto& [fd, d] : devs_)
    if (d.path == path) return;
  int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return;
  libevdev* ev = nullptr;
  if (libevdev_new_from_fd(fd, &ev) < 0) { close(fd); return; }

  // Only external devices: USB and Bluetooth. This leaves out the iMac's ACPI power button,
  // lid/video-bus events and the like.
  int bus = libevdev_get_id_bustype(ev);
  bool external = test_devices_ ? bus == BUS_VIRTUAL : bus == BUS_USB || bus == BUS_BLUETOOTH;
  bool pointer = libevdev_has_event_code(ev, EV_REL, REL_X) && libevdev_has_event_code(ev, EV_REL, REL_Y) &&
                 libevdev_has_event_code(ev, EV_KEY, BTN_LEFT);
  bool keys = false;
  uint16_t p, u;
  for (unsigned c = 1; c < BTN_MISC && !keys; c++)
    keys = libevdev_has_event_code(ev, EV_KEY, c) && evdev_to_hid(c, &p, &u);
  if (!external || !(pointer || keys)) {
    libevdev_free(ev);
    close(fd);
    return;
  }

  Dev d;
  d.fd = fd; d.ev = ev; d.path = path;
  d.name = libevdev_get_name(ev) ? libevdev_get_name(ev) : "?";
  d.hires_v = libevdev_has_event_code(ev, EV_REL, REL_WHEEL_HI_RES);
  d.hires_h = libevdev_has_event_code(ev, EV_REL, REL_HWHEEL_HI_RES);
  if (active_) libevdev_grab(ev, LIBEVDEV_GRAB);
  fprintf(stderr, "input: + %s \"%s\"%s%s\n", path.c_str(), d.name.c_str(), keys ? " keys" : "", pointer ? " pointer" : "");
  devs_[fd] = d;
}

void Input::close_dev(int fd) {
  auto it = devs_.find(fd);
  if (it == devs_.end()) return;
  Dev& d = it->second;
  fprintf(stderr, "input: - %s \"%s\"\n", d.path.c_str(), d.name.c_str());
  if (rep_fd_ == fd) rep_fd_ = -1;
  // Unplugged, switched off or out of range with keys held: the device will never report their release.
  // Without this the MacBook keeps those keys down (a stuck key, or a stuck Shift or Command).
  if (active_ && !d.down.empty()) {
    for (auto& [code, when] : d.down) send_key(code, 0);
    fprintf(stderr, "input: \"%s\" went away with %zu key(s) held; released them on the MacBook\n", d.name.c_str(),
            d.down.size());
  }
  libevdev_free(it->second.ev);  // closing the fd also ends a grab
  close(fd);
  devs_.erase(it);
}

void Input::on_inotify() {
  alignas(inotify_event) char buf[4096];
  ssize_t n;
  while ((n = read(inotify_fd_, buf, sizeof buf)) > 0) {
    for (char* p = buf; p < buf + n;) {
      auto* e = (inotify_event*)p;
      p += sizeof(inotify_event) + e->len;
      if (!e->len || strncmp(e->name, "event", 5)) continue;
      std::string path = std::string(kDir) + "/" + e->name;
      if (e->mask & IN_DELETE) {
        for (auto& [fd, d] : devs_)
          if (d.path == path) { close_dev(fd); break; }
      } else {
        try_open(path);  // IN_CREATE, or IN_ATTRIB once udev has set permissions
      }
    }
  }
}

void Input::apply_active(bool on) {
  if (on == active_) return;
  active_ = on;
  for (auto& [fd, d] : devs_) {
    libevdev_grab(d.ev, on ? LIBEVDEV_GRAB : LIBEVDEV_UNGRAB);
    d.dx = d.dy = d.wv = d.wh = 0;
    d.down.clear();  // the MacBook lets go of everything itself when a session ends
  }
  rep_fd_ = -1;
  fprintf(stderr, "input: %s\n", on ? "grabbed, forwarding to the MacBook" : "released to the iMac console");
}

void Input::on_readable(int fd) {
  auto it = devs_.find(fd);
  if (it == devs_.end()) return;
  Dev& d = it->second;
  input_event ev;
  unsigned flag = LIBEVDEV_READ_FLAG_NORMAL;
  for (;;) {
    int rc = libevdev_next_event(d.ev, flag, &ev);
    if (rc == LIBEVDEV_READ_STATUS_SYNC) { flag = LIBEVDEV_READ_FLAG_SYNC; handle(d, ev.type, ev.code, ev.value); continue; }
    if (rc == LIBEVDEV_READ_STATUS_SUCCESS) { handle(d, ev.type, ev.code, ev.value); continue; }
    if (rc == -EAGAIN && flag == LIBEVDEV_READ_FLAG_SYNC) { flag = LIBEVDEV_READ_FLAG_NORMAL; continue; }
    if (rc == -EAGAIN) return;
    close_dev(fd);  // -ENODEV: unplugged
    return;
  }
}

static uint8_t button_number(uint16_t code) {
  switch (code) {
    case BTN_LEFT: return 1;
    case BTN_RIGHT: return 2;
    case BTN_MIDDLE: return 3;
    case BTN_SIDE: case BTN_BACK: return 4;
    case BTN_EXTRA: case BTN_FORWARD: return 5;
  }
  return 0;
}

static bool is_modifier(uint16_t page, uint16_t usage) { return page == 0x07 && usage >= 0xE0 && usage <= 0xE7; }

// One INPUT_KEY or INPUT_BUTTON for an evdev code. Codes with no HID usage are not sent.
void Input::send_key(uint16_t code, uint8_t state) {
  if (uint8_t b = button_number(code)) {
    if (state == 2) return;
    uint8_t pl[4] = {b, state, 0, 0};
    emit_(proto::INPUT_BUTTON, pl, sizeof pl);
    return;
  }
  uint16_t page, usage;
  if (!evdev_to_hid(code, &page, &usage)) return;
  uint8_t pl[8] = {};
  proto::put_u16(pl, page);
  proto::put_u16(pl + 2, usage);
  pl[4] = state;
  emit_(proto::INPUT_KEY, pl, sizeof pl);
}

void Input::handle(Dev& d, uint16_t type, uint16_t code, int32_t value) {
  if (!active_) {  // the iMac's own waiting screen
    if (!local_) return;
    if (type == EV_REL && code == REL_X) d.dx += value;
    else if (type == EV_REL && code == REL_Y) d.dy += value;
    else if (type == EV_KEY && value != 2) local_(0, 0, code, value);
    else if (type == EV_SYN && code == SYN_REPORT && (d.dx || d.dy)) {
      local_(d.dx, d.dy, 0, 0);
      d.dx = d.dy = 0;
    }
    return;
  }
  if (type == EV_KEY) {
    if (value == 2) return;  // the kernel's autorepeat: ours follows the MacBook's settings (tick_repeat)
    const bool button = button_number(code) != 0;
    uint16_t page = 0, usage = 0;
    if (!button && !evdev_to_hid(code, &page, &usage)) return;
    if (button) flush_pointer(d);  // movement before the click is sent before the click
    const uint64_t now = now_us();
    send_key(code, value ? 1 : 0);
    // Trace of modifier keys (never of other keys): what each keyboard itself reported, to tell a
    // keyboard that keeps a modifier down from one that got stuck on the MacBook.
    if (!button && is_modifier(page, usage)) {
      static const char* names[] = {"left Control", "left Shift", "left Alt/Option", "left Windows/Command",
                                    "right Control", "right Shift", "right Alt/Option", "right Windows/Command"};
      fprintf(stderr, "input: \"%s\" %s %s\n", d.name.c_str(), names[usage - 0xE0], value ? "down" : "up");
    }
    if (value) {
      d.down[code] = now;
      // Letters, digits, arrows and so on repeat; modifiers and Caps Lock do not, and do not stop a repeat.
      if (!button && page == 0x07 && !is_modifier(page, usage) && usage != 0x39) {
        int delay = repeat_delay_ms_;
        if (delay > 0) { rep_fd_ = d.fd; rep_code_ = code; rep_next_us_ = now + uint64_t(delay) * 1000; }
        else rep_fd_ = -1;
      }
      return;
    }
    if (rep_fd_ == d.fd && rep_code_ == code) rep_fd_ = -1;
    auto held = d.down.find(code);
    if (held == d.down.end()) return;
    // For telling a keyboard that reports a release late (radio trouble, a worn switch) from a network
    // delay: this is how long the keyboard itself said the key was down. Not which key.
    if (!button && !is_modifier(page, usage) && now - held->second >= 3000000 && now - long_hold_logged_us_ >= 30000000) {
      long_hold_logged_us_ = now;
      fprintf(stderr, "input: \"%s\" reported a key held for %.1f s\n", d.name.c_str(), (now - held->second) / 1e6);
    }
    d.down.erase(held);
  } else if (type == EV_REL) {
    switch (code) {
      case REL_X: d.dx += value; break;
      case REL_Y: d.dy += value; break;
      // Positive = wheel turned away from the user (scroll up) / to the right, 120 per notch.
      case REL_WHEEL: if (!d.hires_v) d.wv += value * 120; break;
      case REL_HWHEEL: if (!d.hires_h) d.wh += value * 120; break;
      case REL_WHEEL_HI_RES: d.wv += value; break;
      case REL_HWHEEL_HI_RES: d.wh += value; break;
    }
  } else if (type == EV_SYN && code == SYN_REPORT) {
    flush_pointer(d);
  }
}

void Input::flush_pointer(Dev& d) {
  if (!d.dx && !d.dy && !d.wv && !d.wh) return;
  uint8_t pl[16];
  proto::put_u32(pl, uint32_t(d.dx));
  proto::put_u32(pl + 4, uint32_t(d.dy));
  proto::put_u32(pl + 8, uint32_t(d.wv));
  proto::put_u32(pl + 12, uint32_t(d.wh));
  emit_(proto::INPUT_POINTER_REL, pl, sizeof pl);
  d.dx = d.dy = d.wv = d.wh = 0;
}
