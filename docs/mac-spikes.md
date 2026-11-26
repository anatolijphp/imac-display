# Mac-side spikes (M1)

Measured by the MacBook session on 2026-11-26. MacBook Pro (Mac17,2), Apple M5, 16 GB, macOS 27.2 (26B123). Built with the Command Line Tools only (Swift 6.4.2, no Xcode). The code is in `mac/`; every number below comes from `mac/.build/release/imd` unless another command is named.

## Summary

| Spike | Result |
|---|---|
| Virtual display | **Works.** A 2560×1440 HiDPI display (5120×2880 pixels, 60 Hz) appears and disappears on demand. Private API, no entitlement needed. |
| Screen capture | **Works** at 60 pictures per second, at 4K and at 5K. Needs the user's Screen Recording permission once. |
| Low-latency encode | **4K at 60 works, with no headroom** (about 16.5 ms per picture). **5K at 60 does not**: the hardware encoder reaches about 36 pictures per second at 5120×2880. |
| Build and sign without Xcode | **Works.** `swift build` plus a shell script give a signed `.app`. Ad-hoc signing has one cost: macOS asks for the Screen Recording permission again after every rebuild. |
| Whole chain over `PROTOCOL.md` | **Works** against a stand-in iMac (`mac/test/fake_imac.py`): handshake, video in one and in four tiles, keyframe on request. Not yet run against the real receiver. |

What this means for the plan: M3 (one 4K stream of a 2560×1440 HiDPI desktop) is feasible as designed. Native 5K (M7) cannot be "four tiles, every frame": it needs the changed-tiles-only scheme now written into `PROTOCOL.md`, or it runs at about 36 pictures per second when the whole screen moves.

## 1. Virtual display

- The classes `CGVirtualDisplay`, `CGVirtualDisplayDescriptor`, `CGVirtualDisplaySettings` and `CGVirtualDisplayMode` exist in CoreGraphics on macOS 27.2 and link from the SDK. They are in no public header; `mac/Sources/CVirtualDisplay/include/CVirtualDisplay.h` declares them. Selectors and argument types were read from the Objective-C runtime (`method_getTypeEncoding`): every integer is a 32-bit `unsigned int`.
- `imd vdisplay`: `displayID 126`, current mode `2560x1440 points, 5120x2880 pixels, 60.0 Hz`, `NSScreen "iMac Display", scale 2.0, max 60 fps`. macOS also offers scaled modes on it (1920×1080, 1280×720 HiDPI, 5120×2880 at 1×, and others).
- Releasing the object removes the display: `removed 0.1 s after release`. (`CGDisplayIsOnline` answers nonzero for an ID that no longer exists, so check `CGGetOnlineDisplayList`.)
- Works from a plain command-line binary and from the ad-hoc-signed app bundle.
- Risk: private API. It has been stable for years and other display tools depend on it, but Apple can change it in any update.

## 2. Screen capture

ScreenCaptureKit (`SCStream`) on the virtual display, NV12 pixel buffers, test content of two columns of scrolling text and a moving box (`TestScene.swift`).

| Capture size | `minimumFrameInterval` | New pictures per second | Time between pictures, ms (p50 / p95 / max) |
|---|---|---|---|
| 3840×2160 (scaled down from 5120×2880 by the capture) | 1/60 s | 58.5 | 17.3 / 19.0 / 74.8 |
| 3840×2160 | 1/120 s | **59.8** | 16.6 / 18.2 / 81.9 |
| 5120×2880 (encoder not keeping up, callback returned at once) | 1/60 s | 58.5–58.8 | 17.3 / 18.6 / 22.3 |

- Asking for exactly 1/60 s loses about one picture in forty. The app asks for a shorter interval and gets every refresh.
- Pictures arrive 5 to 12 ms **before** their stated display time (median; the stamp is the refresh the picture is meant for).
- Nothing arrives while the screen is unchanged. That is what we want: an idle desktop costs nothing.
- **Permission:** the command-line tool inherits the terminal's Screen Recording permission. The app bundle is its own program: its first run was refused and macOS showed its prompt (`screen capture is not allowed for this program yet`).

## 3. Encode

VideoToolbox hardware H.264, High profile, CABAC, no frame reordering, 40 Mbit/s average unless noted, keyframes only on request. "Low latency" is `EnableLowLatencyRateControl` (encoder `…h264.rtvc`); "normal" is the standard encoder (`…ave.avc`).

| Case | Pictures per second | Encode time per tile, ms (p50 / p95) | Capture callback → all tiles encoded, ms (p50 / p95) |
|---|---|---|---|
| 4K, one stream, low latency | 58.5 (59.8 with the shorter capture interval) | 16.9 / 19.5 (16.3 / 17.5) | 16.9 / 19.5 |
| 4K, one stream, normal | 58.5 | 21.6 / 51.1 | 21.7 / 51.2 |
| 5K, one H.264 stream | 0: the encoder opens at 5120×2880 but every frame fails (351 of 351) | | |
| 5K, one HEVC stream, low latency | 36.2 | 27.0 / 27.5 | 27.1 / 27.6 |
| 5K, 4 tiles of 2560×1440, low latency, one after another | 31.7 | 7.3 / 7.8 | 32.0 / 33.9 |
| 5K, 4 tiles, low latency, side by side | **35.8** | 18.8 / 26.0 | 27.9 / 28.8 |
| 5K, 4 tiles, HEVC, low latency, side by side | 32.8 | 20.4 / 28.2 | 30.3 / 31.4 |
| 5K, 4 tiles, normal | 40.8 | 159.7 / 160.1 (queued) | 178.9 / 179.3 |

- **The ceiling is the encoder hardware: about 500 to 600 megapixels per second**, the same for H.264 and HEVC, for one stream or four. 4K at 60 needs 498, 5K at 60 needs 885. Running four encoders side by side does not add capacity (27.9 ms for four tiles against 4 × 7.3 ms one after another).
- Cross-check with a different program: `ffmpeg -i raw4k.nut -c:v h264_videotoolbox -b:v 40M -bf 0 -f null -` on 240 raw 4K frames ran at **70 fps** (580 megapixels per second); `hevc_videotoolbox` 68 fps.
- Settings that changed nothing: `ExpectedFrameRate` 120, `MaximizePowerEfficiency` off, `RealTime` off. Refused by the low-latency encoder (error −12900): `PrioritizeEncodingSpeedOverQuality`, `MaxFrameDelayCount` 0. One run (4K with `ExpectedFrameRate` 120) produced no output at all; not investigated.
- The low-latency encoder returns each picture before it accepts the next (no queue), which is why its latency is its encode time. The normal encoder is slightly faster overall but holds pictures back.
- Cutting a 5K picture into four tiles (copying, four threads): 2.1 / 2.9 ms.
- Rate control holds the target: 40.4–40.9 Mbit/s measured at 4K. The first keyframe is 560–640 kB, a typical frame 75–85 kB, the largest 377 kB.
- Output checked with `ffprobe`: `h264, High, level 5.2 (4K) / 5.1 (tiles), yuv420p, bt709, tv range, has_b_frames=0`, frame counts equal to the pictures captured. Text in decoded pictures is sharp at both sizes (looked at crops of single frames).

### The same streams decoded on the iMac

Real output of this encoder (scrolling text, harder than the iMac's synthetic clips), copied to the iMac's `/tmp`, timed with `ffmpeg -nostdin -loglevel error -threads N -i CLIP -f null -`, then deleted.

| Clip | Threads | Pictures per second |
|---|---|---|
| 4K, 40 Mbit/s, 351 frames | 1 / 2 / 4 / all 8 | 32 / 52 / 79 / **104** |
| 4 tiles of 2560×1440 at once (25 Mbit/s in total at 36 pictures per second) | 1 each | 54 (slowest of the four) |
| 4 tiles of 2560×1440 at once | 2 each | 70 |

4K at 60 has margin on the iMac only with all cores (the iMac's own synthetic test gave 160). Four tiles with one thread each fall short of 60; two threads each reach 70.

## 4. Build and sign without Xcode

- `cd mac && swift build -c release`: 19 s from clean. Three targets: `CVirtualDisplay` (the private declarations), `IMDKit` (wire format, session, virtual display, capture, encoder, tiles), `imd` (test tool).
- `mac/scripts/build-app.sh` builds `out/iMac Display.app` (bundle ID `local.imac-display.mac`, menu-bar-only, local-network usage text and `_imacdisplay._tcp` declared) and signs it. `codesign --verify --strict`: valid. Started with `open`, it created and removed the virtual display like the command-line tool.
- This Mac has no code-signing certificate (`security find-identity -p codesigning`: 0), so the signature is ad hoc. Consequence: the Screen Recording permission is tied to one exact build. With a certificate (`SIGN_IDENTITY=… build-app.sh`), even a self-signed one, the permission survives rebuilds. **Open decision for the user** before M3: create a self-signed certificate in the login keychain, or use an Apple Developer certificate (the paid membership is needed for M9 anyway).
- Not tried yet: the sound driver (a HAL plug-in in `/Library/Audio/Plug-Ins/HAL`, M4), launch at login.

## 5. Whole chain over the protocol

`mac/test/fake_imac.py` stands in for `imacdisplayd`. `imd pipeline --host localhost --port 7501 …`:

- 4K: 291 pictures sent, 291 received, 2 keyframes (the first, and one after the stand-in's `VIDEO_REQUEST_KEYFRAME`), the received file is valid H.264.
- 5K in four tiles: 568 frames = 142 pictures × 4, 8 keyframes, none out of order, four valid 2560×1440 files.
- `STORAGE_ATTACHED` arriving before `CONFIG_ACK` and an `INPUT_KEY` after it are received and printed.
- Writing one picture to the socket: 0.1 / 0.2 ms (loopback, so this says nothing about the cable).

## 6. First run against the real iMac (over Wi-Fi, testing only)

`imd pipeline --host imac-display.local --lodpi --points 1920x1080 --mbps 10`, receiver on `wlan0`, 2026-11-26.

| Pictures per second | Result on the iMac (its own `VIDEO_STATS`) |
|---|---|
| 30 | **Works.** 28–30 decoded and shown every second for 20 s, 5 dropped in total. Decode 13–14 ms, arrival to screen 30–36 ms. |
| 60 | **Does not work yet.** 2 pictures shown after each keyframe, then nothing. Two receiver bugs (a lost keyframe request; a queue limit that a keyframe always exceeds) and slow decoding inside the receiver; details in `docs/handoff.md`. |

The MacBook side held 60.0 pictures per second for 25 s with a 6 ms encode time at this size. Whether the picture on the iMac looked right was not checked by a person yet.

- `DataRateLimits` (to make keyframes smaller) is accepted by the low-latency encoder and has no effect: a 1080p keyframe stays at about 270 kB at 10 Mbit/s.
- One 30 s run had a 6.3 s gap in the capture on the MacBook (1321 pictures instead of about 1800). It did not happen again in three later runs; cause unknown.

## 7. Where the time goes at 3840×2160 (measured over Wi-Fi, 2026-11-26)

`imd pipeline --host imac-display.local --capture 3840x2160 --capture-fps 120 --seconds 16` with the scrolling-text test picture, sound and input off. MacBook numbers from the tool, iMac numbers from `imacdisplayd`'s own log. Wi-Fi at the time: `iperf3` 119–123 Mbit/s, ping 3.0 / 28.1 / 151.9 ms (min / avg / max).

| Run | MacBook: capture → encoded, ms (p50 / p95) | MacBook: socket write, ms (p50 / p95 / max) | Pictures reaching the iMac per second | iMac: arrival → on screen, ms | iMac: shown / dropped per second |
|---|---|---|---|---|---|
| One stream, 20 Mbit/s | 16.3 / 17.3 | 0.2 / 0.3 / 263 | 59.8 | **55–59** | 53–56 / 4–7 |
| One stream, 40 Mbit/s | 16.3 / 17.4 | 0.5 / **73** / 676 | **45** | 67–73 | 39–40 / 2–5 |
| Four 1920×1080 tiles, 20 Mbit/s | 24.4 / 26.2 (of which cutting 7.5) | 0.3 / 0.5 / 32 | 60.0 | **24** | 56–58 / 2–5 |

What limits what:

1. **The iMac's decoder arrangement for one large stream is the biggest item.** For a tile larger than 2560×1440 the receiver uses frame threads, which hold each picture back about three frames. The same picture as four tiles is decoded side by side with no hold-back: 24 ms instead of 55–59 ms. The app therefore sends pictures larger than 2560×1440 as 2 × 2 tiles; with the app the iMac logged 20–30 ms.
2. **The iMac's page flip** is 16–17 ms of that 24 ms: a picture waits a whole refresh. Open on the iMac side.
3. **The MacBook's encoder**: 16 ms for 3840×2160 whether as one stream or four (the hardware limit of section 3). Cutting into tiles costs another 7.5 ms here, which is more than copying 12 MB should take; not looked into yet.
4. **Wi-Fi** carries 20 Mbit/s without trouble and does not carry 40: the socket backs up (p95 73 ms) and only 45 pictures per second get through. This is the one item the Thunderbolt cable is expected to remove.
5. In every run the iMac drops 2–7 pictures per second ("newest wins" when two arrive close together). Whether that is Wi-Fi's uneven delivery is not known until the cable is tested.

Sum for the tiled case: about 24 ms on the MacBook, a few ms of Wi-Fi, about 24 ms on the iMac: **roughly 50 ms** from the MacBook's screen refresh to the iMac's, against roughly 75–80 ms for one stream. Added up from the parts, not measured end to end.

## Expected delay, not measured

Adding up the parts for 4K: encode about 17 ms, decode on the iMac about 10 ms (104 pictures per second), then up to one refresh (17 ms) to show it. Roughly 30 to 45 ms from the MacBook drawing a picture to the iMac showing it, before any network delay. This is arithmetic from the numbers above, not a measurement; the real figure comes in M3.

## Run it again

    cd mac && swift build -c release
    .build/release/imd vdisplay
    .build/release/imd pipeline --capture 3840x2160 --capture-fps 120 --seconds 6 --out /tmp/clip
    .build/release/imd pipeline --cols 2 --rows 2 --seconds 6
    test/fake_imac.py --port 7501 --out /tmp/fake &
    .build/release/imd pipeline --host localhost --port 7501 --capture 3840x2160 --seconds 5
