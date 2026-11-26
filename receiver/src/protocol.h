// Wire format from PROTOCOL.md, version 0. PROTOCOL.md is the authority; this file follows it.
#pragma once
#include <cstdint>
#include <cstring>

namespace proto {

constexpr int kVersion = 0;
constexpr uint16_t kPort = 7500;
constexpr uint32_t kMaxPayload = 64u << 20;  // 64 MiB
constexpr size_t kHeaderSize = 16;

enum Type : uint8_t {
  HELLO = 0x01, CONFIG = 0x02, CONFIG_ACK = 0x03, PING = 0x04, PONG = 0x05, BYE = 0x06,
  VIDEO_FRAME = 0x10, VIDEO_REQUEST_KEYFRAME = 0x11, VIDEO_STATS = 0x12,
  AUDIO_OUT = 0x20, AUDIO_IN = 0x21, AUDIO_VOLUME = 0x22,
  INPUT_KEY = 0x30, INPUT_POINTER_REL = 0x31, INPUT_BUTTON = 0x32,
  STORAGE_ATTACHED = 0x40, STORAGE_EJECT = 0x41, STORAGE_DETACHED = 0x42,
  DISPLAY_BRIGHTNESS = 0x50, DISPLAY_POWER = 0x51,
};

struct Header {
  uint32_t length = 0;
  uint8_t type = 0;
  uint8_t flags = 0;
  uint16_t tile = 0;
  uint64_t pts_us = 0;
};

// Little-endian helpers (x86_64 is little-endian, but keep the format explicit).
inline void put_u16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
inline void put_u32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
inline void put_u64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = v >> (8 * i); }
inline uint16_t get_u16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline uint32_t get_u32(const uint8_t* p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
inline uint64_t get_u64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }

inline void encode_header(uint8_t* p, const Header& h) {
  put_u32(p, h.length); p[4] = h.type; p[5] = h.flags; put_u16(p + 6, h.tile); put_u64(p + 8, h.pts_us);
}
inline Header decode_header(const uint8_t* p) {
  Header h; h.length = get_u32(p); h.type = p[4]; h.flags = p[5]; h.tile = get_u16(p + 6); h.pts_us = get_u64(p + 8);
  return h;
}

}  // namespace proto
