// Tests input.cpp's key repeat and key release with a virtual keyboard (uinput).
// Run on the iMac as root; it can run beside the receiver: the receiver takes only USB and Bluetooth
// devices, this test takes only virtual ones, and it grabs its keyboard so the console sees nothing.
#include <fcntl.h>
#include <linux/uinput.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "input.h"
#include "net.h"
#include "protocol.h"

struct Got { uint64_t us; uint16_t usage; uint8_t state; };
static std::vector<Got> got;
static int failures = 0;

static int make_keyboard() {
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) { perror("/dev/uinput"); exit(2); }
  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_KEYBIT, KEY_A);
  ioctl(fd, UI_SET_KEYBIT, KEY_LEFTSHIFT);
  uinput_setup s{};
  s.id.bustype = BUS_VIRTUAL;
  s.id.vendor = 0x1234; s.id.product = 0x5678;
  strcpy(s.name, "imd input test keyboard");
  ioctl(fd, UI_DEV_SETUP, &s);
  ioctl(fd, UI_DEV_CREATE);
  return fd;
}

static void key(int fd, int code, int value) {
  input_event e[2]{};
  e[0].type = EV_KEY; e[0].code = code; e[0].value = value;
  e[1].type = EV_SYN; e[1].code = SYN_REPORT;
  (void)!write(fd, e, sizeof e);
}

// Lets `ms` pass while collecting what the Input sends.
static void run(Input& in, int ms) {
  uint64_t end = now_us() + uint64_t(ms) * 1000;
  for (;;) {
    uint64_t now = now_us();
    if (now >= end) break;
    pollfd p{in.notify_fd(), POLLIN, 0};
    if (poll(&p, 1, int((end - now) / 1000) + 1) > 0) in.drain();
  }
  in.drain();
}

static int count(uint16_t usage, uint8_t state) {
  int n = 0;
  for (auto& g : got) n += g.usage == usage && g.state == state;
  return n;
}

static void check(bool ok, const std::string& what) {
  printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) {
    failures++;
    for (auto& g : got) printf("        usage 0x%02x state %d\n", g.usage, g.state);
  }
}

int main() {
  const uint16_t A = 0x04, SHIFT = 0xE1;
  int kb = make_keyboard();
  Input in([](uint8_t type, const void* p, size_t n) {
             if (type != proto::INPUT_KEY || n < 5) return;
             const uint8_t* b = (const uint8_t*)p;
             got.push_back({now_us(), uint16_t(b[2] | b[3] << 8), b[4]});
           },
           [](int, int, uint16_t, int) {}, /*test_devices=*/true);
  run(in, 1500);  // udev sets the device up; the Input finds it
  in.set_active(true);
  in.set_repeat(400, 100);
  run(in, 200);

  got.clear();
  key(kb, KEY_A, 1); run(in, 80); key(kb, KEY_A, 0); run(in, 900);
  check(got.size() == 2 && count(A, 1) == 1 && count(A, 0) == 1, "a tap is one press and one release, no repeat");

  got.clear();
  key(kb, KEY_A, 1); run(in, 1050); key(kb, KEY_A, 0); run(in, 600);
  int r = count(A, 2);
  check(count(A, 1) == 1 && count(A, 0) == 1 && r >= 6 && r <= 7 && got.back().state == 0,
        "held 1.05 s with delay 400 ms, interval 100 ms: 6 or 7 repeats (got " + std::to_string(r) + "), none after the release");
  uint64_t first = 0;
  for (auto& g : got) if (g.state == 2) { first = g.us - got.front().us; break; }
  check(first >= 395000 && first <= 430000, "the first repeat comes after the delay (" + std::to_string(first / 1000) + " ms)");

  got.clear();
  key(kb, KEY_A, 1); run(in, 450); key(kb, KEY_LEFTSHIFT, 1); run(in, 400); int before = count(A, 2);
  key(kb, KEY_LEFTSHIFT, 0); run(in, 50); key(kb, KEY_A, 0); run(in, 300);
  check(before >= 4 && count(SHIFT, 2) == 0 && count(SHIFT, 1) == 1 && count(SHIFT, 0) == 1,
        "Shift pressed during a repeat does not stop it and does not repeat itself");

  in.set_repeat(0, 0);
  got.clear();
  key(kb, KEY_A, 1); run(in, 700); key(kb, KEY_A, 0); run(in, 100);
  check(got.size() == 2, "no repeats when the MacBook did not ask for them");

  in.set_repeat(400, 100);
  got.clear();
  key(kb, KEY_A, 1); key(kb, KEY_LEFTSHIFT, 1); run(in, 150);
  ioctl(kb, UI_DEV_DESTROY);  // the keyboard is unplugged with both keys down
  close(kb);
  run(in, 1200);
  check(count(A, 0) == 1 && count(SHIFT, 0) == 1 && count(A, 2) == 0,
        "a keyboard that goes away with keys held: both are released, nothing repeats");

  printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
