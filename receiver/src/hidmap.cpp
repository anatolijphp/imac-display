// Linux evdev key codes -> USB HID usages.
// Built by inverting the kernel's HID keyboard table (drivers/hid/hid-input.c, hid_keyboard[]),
// plus the consumer-page keys the MacBook handles itself (volume, media, brightness).
#include "input.h"

#include <linux/input-event-codes.h>

namespace {

struct Entry { uint16_t code; uint16_t page; uint16_t usage; };

constexpr uint16_t KB = 0x07, CONS = 0x0C;

const Entry kMap[] = {
  // Letters (HID 0x04-0x1D)
  {KEY_A, KB, 0x04}, {KEY_B, KB, 0x05}, {KEY_C, KB, 0x06}, {KEY_D, KB, 0x07}, {KEY_E, KB, 0x08},
  {KEY_F, KB, 0x09}, {KEY_G, KB, 0x0A}, {KEY_H, KB, 0x0B}, {KEY_I, KB, 0x0C}, {KEY_J, KB, 0x0D},
  {KEY_K, KB, 0x0E}, {KEY_L, KB, 0x0F}, {KEY_M, KB, 0x10}, {KEY_N, KB, 0x11}, {KEY_O, KB, 0x12},
  {KEY_P, KB, 0x13}, {KEY_Q, KB, 0x14}, {KEY_R, KB, 0x15}, {KEY_S, KB, 0x16}, {KEY_T, KB, 0x17},
  {KEY_U, KB, 0x18}, {KEY_V, KB, 0x19}, {KEY_W, KB, 0x1A}, {KEY_X, KB, 0x1B}, {KEY_Y, KB, 0x1C},
  {KEY_Z, KB, 0x1D},
  // Digits row (0x1E-0x27)
  {KEY_1, KB, 0x1E}, {KEY_2, KB, 0x1F}, {KEY_3, KB, 0x20}, {KEY_4, KB, 0x21}, {KEY_5, KB, 0x22},
  {KEY_6, KB, 0x23}, {KEY_7, KB, 0x24}, {KEY_8, KB, 0x25}, {KEY_9, KB, 0x26}, {KEY_0, KB, 0x27},
  // 0x28-0x38
  {KEY_ENTER, KB, 0x28}, {KEY_ESC, KB, 0x29}, {KEY_BACKSPACE, KB, 0x2A}, {KEY_TAB, KB, 0x2B},
  {KEY_SPACE, KB, 0x2C}, {KEY_MINUS, KB, 0x2D}, {KEY_EQUAL, KB, 0x2E}, {KEY_LEFTBRACE, KB, 0x2F},
  {KEY_RIGHTBRACE, KB, 0x30}, {KEY_BACKSLASH, KB, 0x31}, {KEY_SEMICOLON, KB, 0x33},
  {KEY_APOSTROPHE, KB, 0x34}, {KEY_GRAVE, KB, 0x35}, {KEY_COMMA, KB, 0x36}, {KEY_DOT, KB, 0x37},
  {KEY_SLASH, KB, 0x38}, {KEY_CAPSLOCK, KB, 0x39},
  // F1-F12 (0x3A-0x45)
  {KEY_F1, KB, 0x3A}, {KEY_F2, KB, 0x3B}, {KEY_F3, KB, 0x3C}, {KEY_F4, KB, 0x3D}, {KEY_F5, KB, 0x3E},
  {KEY_F6, KB, 0x3F}, {KEY_F7, KB, 0x40}, {KEY_F8, KB, 0x41}, {KEY_F9, KB, 0x42}, {KEY_F10, KB, 0x43},
  {KEY_F11, KB, 0x44}, {KEY_F12, KB, 0x45},
  // 0x46-0x52
  {KEY_SYSRQ, KB, 0x46}, {KEY_SCROLLLOCK, KB, 0x47}, {KEY_PAUSE, KB, 0x48}, {KEY_INSERT, KB, 0x49},
  {KEY_HOME, KB, 0x4A}, {KEY_PAGEUP, KB, 0x4B}, {KEY_DELETE, KB, 0x4C}, {KEY_END, KB, 0x4D},
  {KEY_PAGEDOWN, KB, 0x4E}, {KEY_RIGHT, KB, 0x4F}, {KEY_LEFT, KB, 0x50}, {KEY_DOWN, KB, 0x51},
  {KEY_UP, KB, 0x52},
  // Keypad (0x53-0x63, 0x67, 0x85)
  {KEY_NUMLOCK, KB, 0x53}, {KEY_KPSLASH, KB, 0x54}, {KEY_KPASTERISK, KB, 0x55}, {KEY_KPMINUS, KB, 0x56},
  {KEY_KPPLUS, KB, 0x57}, {KEY_KPENTER, KB, 0x58}, {KEY_KP1, KB, 0x59}, {KEY_KP2, KB, 0x5A},
  {KEY_KP3, KB, 0x5B}, {KEY_KP4, KB, 0x5C}, {KEY_KP5, KB, 0x5D}, {KEY_KP6, KB, 0x5E}, {KEY_KP7, KB, 0x5F},
  {KEY_KP8, KB, 0x60}, {KEY_KP9, KB, 0x61}, {KEY_KP0, KB, 0x62}, {KEY_KPDOT, KB, 0x63},
  {KEY_KPEQUAL, KB, 0x67}, {KEY_KPCOMMA, KB, 0x85},
  // ISO key next to left shift, menu key, power
  {KEY_102ND, KB, 0x64}, {KEY_COMPOSE, KB, 0x65}, {KEY_POWER, KB, 0x66},
  // F13-F24 (0x68-0x73)
  {KEY_F13, KB, 0x68}, {KEY_F14, KB, 0x69}, {KEY_F15, KB, 0x6A}, {KEY_F16, KB, 0x6B}, {KEY_F17, KB, 0x6C},
  {KEY_F18, KB, 0x6D}, {KEY_F19, KB, 0x6E}, {KEY_F20, KB, 0x6F}, {KEY_F21, KB, 0x70}, {KEY_F22, KB, 0x71},
  {KEY_F23, KB, 0x72}, {KEY_F24, KB, 0x73},
  // Japanese / Korean keys
  {KEY_RO, KB, 0x87}, {KEY_KATAKANAHIRAGANA, KB, 0x88}, {KEY_YEN, KB, 0x89}, {KEY_HENKAN, KB, 0x8A},
  {KEY_MUHENKAN, KB, 0x8B}, {KEY_KPJPCOMMA, KB, 0x8C}, {KEY_HANGEUL, KB, 0x90}, {KEY_HANJA, KB, 0x91},
  {KEY_KATAKANA, KB, 0x92}, {KEY_HIRAGANA, KB, 0x93}, {KEY_ZENKAKUHANKAKU, KB, 0x94},
  // Modifiers (0xE0-0xE7). Meta = Windows key = Command on the Mac.
  {KEY_LEFTCTRL, KB, 0xE0}, {KEY_LEFTSHIFT, KB, 0xE1}, {KEY_LEFTALT, KB, 0xE2}, {KEY_LEFTMETA, KB, 0xE3},
  {KEY_RIGHTCTRL, KB, 0xE4}, {KEY_RIGHTSHIFT, KB, 0xE5}, {KEY_RIGHTALT, KB, 0xE6}, {KEY_RIGHTMETA, KB, 0xE7},
  // Consumer page: volume, media, brightness, eject
  {KEY_MUTE, CONS, 0xE2}, {KEY_VOLUMEUP, CONS, 0xE9}, {KEY_VOLUMEDOWN, CONS, 0xEA},
  {KEY_PLAYPAUSE, CONS, 0xCD}, {KEY_PLAY, CONS, 0xB0}, {KEY_PAUSECD, CONS, 0xB1},
  {KEY_NEXTSONG, CONS, 0xB5}, {KEY_PREVIOUSSONG, CONS, 0xB6}, {KEY_STOPCD, CONS, 0xB7},
  {KEY_EJECTCD, CONS, 0xB8}, {KEY_FASTFORWARD, CONS, 0xB3}, {KEY_REWIND, CONS, 0xB4},
  {KEY_BRIGHTNESSUP, CONS, 0x6F}, {KEY_BRIGHTNESSDOWN, CONS, 0x70},
};

}  // namespace

bool evdev_to_hid(uint16_t code, uint16_t* page, uint16_t* usage) {
  static uint32_t table[KEY_MAX + 1];  // (page << 16) | usage, 0 = unmapped
  static bool built = false;
  if (!built) {
    for (const auto& e : kMap) table[e.code] = (uint32_t(e.page) << 16) | e.usage;
    built = true;
  }
  if (code > KEY_MAX || !table[code]) return false;
  *page = table[code] >> 16;
  *usage = table[code] & 0xFFFF;
  return true;
}
