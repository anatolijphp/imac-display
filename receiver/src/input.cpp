#include "input.h"

#include <dirent.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "protocol.h"

static const char* kDir = "/dev/input";

Input::Input(Emit emit, Local local) : out_emit_(std::move(emit)), out_local_(std::move(local)) {
  out_efd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  wake_efd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  auto notify = [this] { uint64_t one = 1; (void)!write(out_efd_, &one, sizeof one); };
  emit_ = [this, notify](uint8_t t, const void* p, size_t n) {
    std::lock_guard<std::mutex> l(mu_);
    queue_.push_back(Msg{false, t, std::vector<uint8_t>((const uint8_t*)p, (const uint8_t*)p + n), 0, 0, 0, 0});
    notify();
  };
  local_ = [this, notify](int dx, int dy, uint16_t code, int value) {
    std::lock_guard<std::mutex> l(mu_);
    // Each report on its own (not summed): the pointer's acceleration depends on the size of each.
    queue_.push_back(Msg{true, 0, {}, dx, dy, code, value});
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
    if (poll(p.data(), p.size(), 500) <= 0) continue;
    if (p[1].revents) { uint64_t n; (void)!read(wake_efd_, &n, sizeof n); }
    if (p[0].revents) on_inotify();
    for (size_t i = 2; i < p.size(); i++)
      if (p[i].revents) on_readable(p[i].fd);
  }
}

void Input::drain() {
  uint64_t n;
  (void)!read(out_efd_, &n, sizeof n);
  std::vector<Msg> q;
  {
    std::lock_guard<std::mutex> l(mu_);
    q.swap(queue_);
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
  bool external = bus == BUS_USB || bus == BUS_BLUETOOTH;
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
  fprintf(stderr, "input: - %s \"%s\"\n", it->second.path.c_str(), it->second.name.c_str());
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
  }
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
    if (value == 2) return;  // autorepeat: the MacBook does its own key repeat
    if (uint8_t b = button_number(code)) {
      flush_pointer(d);  // movement before the click is sent before the click
      uint8_t pl[4] = {b, uint8_t(value ? 1 : 0), 0, 0};
      emit_(proto::INPUT_BUTTON, pl, sizeof pl);
      return;
    }
    uint16_t page, usage;
    if (!evdev_to_hid(code, &page, &usage)) return;
    uint8_t pl[8] = {};
    proto::put_u16(pl, page);
    proto::put_u16(pl + 2, usage);
    pl[4] = value ? 1 : 0;
    emit_(proto::INPUT_KEY, pl, sizeof pl);
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
