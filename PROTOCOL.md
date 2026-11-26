# iMac Display protocol, version 0 (draft)

The wire format between the MacBook app and `imacdisplayd` on the iMac. Draft until video works end to end (M3); expect changes. Only the MacBook session edits this file. The iMac session requests changes in `docs/handoff.md`.

## Link and discovery

- The link is Thunderbolt networking: `thunderbolt0` on the iMac, `bridge0` on the MacBook, IPv4 link-local (169.254.0.0/16) and IPv6 link-local addresses.
- The iMac listens on TCP port **7500**, bound to the Thunderbolt interface only.
- The iMac advertises DNS-SD service `_imacdisplay._tcp` on that interface. TXT records: `proto=0`, `model=iMac17,1`, `panel=5120x2880`.
- The MacBook browses for the service on `bridge0` and connects when it appears. There is no authentication in version 0: the link is a point-to-point cable.
- **Testing only:** `imacdisplayd --iface wlan0` listens on Wi-Fi, and the Mac side can be told to connect to a named host (`imd … --host imac-display.local`; the app gets the same as a hidden debug setting). Thunderbolt stays the only link of the product.

## Connections

The MacBook opens two TCP connections to port 7500. `TCP_NODELAY` is set on both.

| Channel | Direction | Carries |
|---|---|---|
| `ctrl` | both ways | handshake, configuration, audio, input, storage, display control |
| `video` | MacBook → iMac | video frames only |

Video has its own connection so a large frame never delays audio or input.

## Frame format

Every message on both connections is one frame: a 16-byte header, then the payload. All integers are little-endian.

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 | `length` | payload size in bytes (header not included); at most 64 MiB |
| 4 | 1 | `type` | message type, see below |
| 5 | 1 | `flags` | per-type flags, otherwise 0 |
| 6 | 2 | `tile` | tile index for video frames, otherwise 0 |
| 8 | 8 | `pts_us` | sender's monotonic clock in microseconds |

JSON payloads are UTF-8 without a trailing NUL. Unknown JSON keys are ignored. An unknown `type` is skipped using `length`.

## Message types

### Session (ctrl unless noted)

| Type | Name | Direction | Payload |
|---|---|---|---|
| 0x01 | `HELLO` | both, first frame on each connection | JSON |
| 0x02 | `CONFIG` | MacBook → iMac | JSON |
| 0x03 | `CONFIG_ACK` | iMac → MacBook | JSON `{"ok":true}` or `{"ok":false,"error":"…"}` |
| 0x04 | `PING` | either | 8 bytes, opaque |
| 0x05 | `PONG` | either | the 8 bytes from the `PING` |
| 0x06 | `BYE` | either | JSON `{"reason":"…"}` |

`HELLO` from the MacBook:
```json
{"proto":0,"role":"mac","channel":"ctrl","session":"3f1c…-uuid","name":"MacBook Pro"}
```
The `video` connection sends the same `session` with `"channel":"video"`.

`HELLO` from the iMac (on `ctrl`; on `video` it replies `{"proto":0,"role":"imac"}`):
```json
{"proto":0,"role":"imac",
 "panel":{"w":5120,"h":2880,"hz":60},
 "decode":{"codecs":["h264"],"max_w":3840,"max_h":2160,"max_tiles":4,"hw":false},
 "audio":{"out":true,"in":true},
 "features":["input","storage","brightness"]}
```
`decode` states what the iMac measured it can decode at 60 fps. The MacBook never asks for more. `max_w`×`max_h` is the largest single stream (one tile); `hw` says whether the GPU decodes (on this iMac it does not, see `docs/hardware-validation.md`). `features` and `audio` list only what the receiver has implemented; the MacBook does not use anything that is missing.

`CONFIG`:
```json
{"video":{"codec":"h264","fps":60,"w":3840,"h":2160,
          "tiles":[{"id":0,"x":0,"y":0,"w":3840,"h":2160}],
          "scale":"fit"},
 "audio_out":{"rate":48000,"ch":2,"fmt":"s16le"},
 "audio_in":{"enabled":true,"rate":48000,"ch":1,"fmt":"s16le"}}
```
`w`×`h` is the full picture; each tile is a rectangle inside it, encoded as an independent stream. The iMac scales the full picture to the panel (`fit` keeps aspect ratio). Native 5K is the same message with `w`=5120, `h`=2880 and four 2560×1440 tiles. A new `CONFIG` may be sent at any time; the next video frame is then a keyframe.

A protocol mismatch is answered with `BYE` and a closed connection.

### Video

| Type | Name | Channel, direction | Payload |
|---|---|---|---|
| 0x10 | `VIDEO_FRAME` | video, MacBook → iMac | one access unit of one tile, H.264 Annex B (start codes) |
| 0x11 | `VIDEO_REQUEST_KEYFRAME` | ctrl, iMac → MacBook | empty |
| 0x12 | `VIDEO_STATS` | ctrl, iMac → MacBook, once per second | JSON |

`VIDEO_FRAME` flags: bit 0 = keyframe (IDR, with SPS and PPS in the payload); bit 1 = last tile sent for this picture. All tiles of one picture carry the same `pts_us` and are sent in ascending tile order. The iMac shows a picture at the next screen refresh after its last tile is decoded; it does not buffer pictures.

**A picture may leave tiles out.** A tile whose part of the screen did not change is not sent; the iMac keeps showing that tile's last decoded picture. The frame with bit 1 set is the last one sent for that `pts_us`, whichever tile it is. Each tile is its own H.264 stream, so a tile that was left out simply continues from its previous frame. With a single tile this changes nothing. (Reason: the MacBook's encoder cannot encode all of 5120×2880 sixty times a second, see `docs/mac-spikes.md`.)

Streams are H.264 High profile, 4:2:0, 8 bit, BT.709 video range, with no B-frames. The encoder sends a keyframe only at the start, after a `CONFIG`, and when asked: there are no periodic keyframes. After a `VIDEO_REQUEST_KEYFRAME` the next picture has a keyframe for every tile. The iMac sends `VIDEO_REQUEST_KEYFRAME` after a decode error.

`VIDEO_STATS`: `{"decoded":60,"dropped":0,"decode_ms":4.1,"present_ms":9.8}` (per-second counts and averages).

### Audio (ctrl)

| Type | Name | Direction | Payload |
|---|---|---|---|
| 0x20 | `AUDIO_OUT` | MacBook → iMac | interleaved PCM as set in `CONFIG.audio_out`, 10 ms per frame |
| 0x21 | `AUDIO_IN` | iMac → MacBook | interleaved PCM as set in `CONFIG.audio_in`, 10 ms per frame |
| 0x22 | `AUDIO_VOLUME` | MacBook → iMac | JSON `{"volume":0.0–1.0,"mute":false}` |

Audio is uncompressed. The receiver keeps a jitter buffer of 20 to 40 ms.

### Input (ctrl, iMac → MacBook)

| Type | Name | Payload (bytes) |
|---|---|---|
| 0x30 | `INPUT_KEY` | `u16 usage_page`, `u16 usage`, `u8 down` (1 or 0), 3 pad. USB HID usages: page 0x07 keyboard, 0x0C consumer (media keys) |
| 0x31 | `INPUT_POINTER_REL` | `i32 dx`, `i32 dy` (raw device counts, no acceleration), `i32 wheel_v`, `i32 wheel_h` (120 per notch) |
| 0x32 | `INPUT_BUTTON` | `u8 button` (1 left, 2 right, 3 middle, 4, 5), `u8 down`, 2 pad |

The MacBook applies its own pointer acceleration and key repeat. Key autorepeat is not sent.

- Wheel: positive `wheel_v` = wheel turned away from the user (content scrolls up), positive `wheel_h` = right. 120 per notch; high-resolution wheels send smaller steps in the same units.
- Modifier keys are ordinary page 0x07 usages (0xE0–0xE7). A PC keyboard's Windows key is 0xE3/0xE7 (GUI), which the MacBook treats as Command.
- Page 0x0C carries volume up/down, mute, play/pause, next/previous track, brightness up/down and eject.
- `INPUT_*` messages can arrive at any time after `CONFIG_ACK`.

### Storage (ctrl, JSON)

| Type | Name | Direction | Payload |
|---|---|---|---|
| 0x40 | `STORAGE_ATTACHED` | iMac → MacBook | see below |
| 0x41 | `STORAGE_EJECT` | MacBook → iMac | `{"id":"…"}`: the MacBook has unmounted the share |
| 0x42 | `STORAGE_DETACHED` | iMac → MacBook | `{"id":"…"}`: the drive is unmounted and safe to remove, or was pulled |

`STORAGE_ATTACHED`:
```json
{"id":"7a0e…","label":"KINGSTON","fs":"exfat","size":31914983424,"read_only":false,
 "share":"usb-7a0e","user":"imac","password":"per-session-random"}
```
The MacBook mounts `smb://imac@<iMac link address>/<share>` (SMB3, port 445, reachable on the session's interface only) with those credentials. The password is generated per session and only travels over the cable.

- One message per **partition**: a drive with several partitions gives several volumes.
- `size` is the size of the filesystem in bytes.
- The iMac announces drives that are already plugged in right after its `HELLO`, so `STORAGE_ATTACHED` can arrive **before** `CONFIG_ACK`. The MacBook accepts storage messages at any point after the iMac's `HELLO`.
- After `STORAGE_EJECT` the iMac unmounts, powers the drive off and answers `STORAGE_DETACHED`. The drive has to be replugged to appear again.
- The iMac's own macOS data volume is offered the same way with `"id":"imac-macos"`, `"fs":"apfs"`, `"read_only":true`; `size` is the size of its APFS container. `STORAGE_EJECT` on it is answered with `STORAGE_DETACHED` and it is offered again in the next session.

### Display control (ctrl, MacBook → iMac, JSON)

| Type | Name | Payload |
|---|---|---|
| 0x50 | `DISPLAY_BRIGHTNESS` | `{"level":0.0–1.0}` |
| 0x51 | `DISPLAY_POWER` | `{"on":true}` or `{"on":false}` (panel off while the MacBook sleeps) |

## Session flow

1. MacBook connects `ctrl`, both sides send `HELLO`.
2. MacBook sends `CONFIG`, iMac answers `CONFIG_ACK`.
3. MacBook connects `video`, both sides send `HELLO`, then `VIDEO_FRAME`s start with a keyframe.
4. Either side sends `PING` once per second on `ctrl`. If nothing at all arrives on `ctrl` for 3 seconds, the session is dead: both sides close both connections. The iMac returns to its waiting screen and turns the panel off after a few minutes without a session.
5. A clean shutdown is a `BYE`.

## Open points for version 1

- Hardware cursor drawn locally on the iMac for zero-lag pointer movement.
- Pointer position feedback so the iMac's mouse can leave the iMac's screen.
- Chroma 4:4:4 if text edges show colour fringing at 4:2:0.
