#include "input.h"

#include <dirent.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "protocol.h"

static const char* kDir = "/dev/input";

Input::Input(Emit emit) : emit_(std::move(emit)) {
  inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  inotify_add_watch(inotify_fd_, kDir, IN_CREATE | IN_ATTRIB | IN_DELETE);
  if (DIR* d = opendir(kDir)) {
    while (dirent* e = readdir(d))
      if (!strncmp(e->d_name, "event", 5)) try_open(std::string(kDir) + "/" + e->d_name);
    closedir(d);
  }
}

Input::~Input() {
  while (!devs_.empty()) close_dev(devs_.begin()->first);
  if (inotify_fd_ >= 0) close(inotify_fd_);
}

std::vector<int> Input::fds() const {
  std::vector<int> v;
  for (auto& [fd, d] : devs_) v.push_back(fd);
  return v;
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

void Input::set_active(bool on) {
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
  if (!active_) return;
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
