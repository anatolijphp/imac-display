// Keyboards and pointers plugged into the iMac, forwarded to the MacBook as INPUT_* messages.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct libevdev;

// Maps a Linux key code to a USB HID (usage page, usage). Returns false for keys we do not forward.
bool evdev_to_hid(uint16_t code, uint16_t* page, uint16_t* usage);

class Input {
 public:
  // emit(type, payload, len) sends one INPUT_* message.
  using Emit = std::function<void(uint8_t type, const void* payload, size_t len)>;
  // Outside a session the devices are not grabbed and nothing is sent; local() gets pointer motion
  // (dx, dy, code 0) and key/button presses (code, value 1 down / 0 up) for the waiting screen.
  using Local = std::function<void(int dx, int dy, uint16_t code, int value)>;
  Input(Emit emit, Local local);
  ~Input();

  int inotify_fd() const { return inotify_fd_; }
  void on_inotify();
  std::vector<int> fds() const;
  void on_readable(int fd);

  // While active, devices are grabbed (the iMac's own console stops seeing them) and events are sent.
  void set_active(bool on);

 private:
  struct Dev {
    int fd = -1;
    libevdev* ev = nullptr;
    std::string path, name;
    bool hires_v = false, hires_h = false;
    int32_t dx = 0, dy = 0, wv = 0, wh = 0;
  };
  void try_open(const std::string& path);
  void close_dev(int fd);
  void handle(Dev& d, uint16_t type, uint16_t code, int32_t value);
  void flush_pointer(Dev& d);

  Emit emit_;
  Local local_;
  int inotify_fd_ = -1;
  bool active_ = false;
  std::map<int, Dev> devs_;
};
