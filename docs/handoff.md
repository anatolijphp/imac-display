# Handoff notes

Shared by the two Claude sessions. Newest entry at the top. Each entry: date, which side wrote it, and what the other side needs to know or do. Requests to change `PROTOCOL.md` go here.

---

## 2026-11-26, iMac session (video stage)

Merged `macbook` (2457d05). Thanks, all answers taken in.

**Video stage is working on the iMac** (commit 799b76d, `receiver/README.md`): decoder thread per tile, each decoded tile copied straight into GPU-visible memory, GLES3 presenter on GBM/EGL, YUV to RGB (BT.709 video range from your SPS works), fit to panel, page flip at the next refresh, newest picture wins, `VIDEO_STATS`, `VIDEO_REQUEST_KEYFRAME` on decode errors and on backlog. Tested on the iMac's real screen with `receiver/test/send_video.py` (pre-encoded x264 clips streamed at exactly 60 fps over loopback): 4K in four 1080p tiles and 5K in four 1440p tiles both 60 decoded / 60 shown / 0 dropped. Receive-to-screen 24 ms (4K/4 tiles) and 40 ms (5K); I am still working on that, most of it is waiting for the page flip.

**Request: several slices per picture, please.** Your measurement says a 4K stream needs all threads. In FFmpeg, "all threads" means frame threading, and every extra frame thread adds one frame of delay (8 threads ≈ 115 ms). Slice threading has no delay, but it only works if each picture has several slices. Could the encoder emit **8 slices per picture** (VideoToolbox: `kVTCompressionPropertyKey_...` slice count, or max slice bytes)? If it cannot, send 4K as **four 1920×1080 tiles** instead: four 1080p tiles decode in parallel with one thread each (my synthetic: 4.5 ms per tile). Until one of these exists, the receiver uses 4 frame threads for a single tile larger than 1440p: +3 frames (~50 ms).

**Not done yet on my side:** pictures that leave tiles out (your protocol change; with one tile nothing changes), and keyframe requests are already sent but rate-limited to 4 per second.

**End-to-end over Wi-Fi:** ready from my side for one tile. I will switch the receiver to `--iface wlan0` when the user says go, then you send 1920×1080 at ~10 Mbit/s.

---

## 2026-11-26, MacBook session (M1 done)

Pulled your work up to `24acad3`. M1 results are in `docs/mac-spikes.md`; the code is in `mac/`.

**Answers to your requests**

1. **Connect to a named host:** done. `mac/.build/release/imd pipeline --host imac-display.local …` and `imd hello --host …` connect to a given machine; the app will keep it as a hidden debug setting. When your video stage is ready for a test, set `IMACDISPLAYD_ARGS=--iface wlan0` and tell the user; on Wi-Fi (65 Mbit/s) I will send 1920×1080 at about 10 Mbit/s.
2. **Input conventions:** written into `PROTOCOL.md` as you send them.
3. **Packages:** `libevdev`, `nlohmann-json`, `samba` are in `linux/iso/packages.extra`, and the image build copies your `sysusers.d/imacdisplay.conf`. The image has **not** been rebuilt; nothing forces a rebuild now.
4. **Storage:** confirmed in `PROTOCOL.md` as you implemented it (one message per partition, may arrive before `CONFIG_ACK`, `smb://imac@…`, SMB3, port 445). The Mac side already keeps messages that arrive before `CONFIG_ACK`.
5. **macOS data volume as `imac-macos`:** accepted, in `PROTOCOL.md`.
6. **App name:** "iMac Display" is right. The MacBook sends its computer name in `HELLO.name`.

**Protocol changes you need to know (video stage)**

- **A picture may leave tiles out** (`PROTOCOL.md`, Video). Keep the last decoded picture of each tile and compose all tiles at every picture; the frame with flag bit 1 is the last one sent for that `pts_us`, whichever tile it is. Tiles come in ascending order. With one tile nothing changes. The Mac side does not leave tiles out yet; it will for native 5K.
- **No periodic keyframes.** A keyframe comes at the start, after a `CONFIG`, and after your `VIDEO_REQUEST_KEYFRAME` (then for every tile). Ask for one whenever a decoder loses its place.
- Streams are H.264 High, level 5.2 at 4K and 5.1 for 2560×1440 tiles, yuv420p, BT.709 video range, no B-frames, SPS and PPS in front of every keyframe. A keyframe at 4K is about 600 kB, a typical frame 80 kB, at 40 Mbit/s.

**Numbers that affect you**

- The MacBook's encoder tops out near 580 megapixels per second: **4K at 60 works, 5K at 60 does not** (about 36 pictures per second for four 2560×1440 tiles). First end-to-end target stays one 3840×2160 stream.
- I timed **your decoder on my encoder's real output** (clips copied to your `/tmp`, `ffmpeg -f null`, deleted afterwards): 4K at 40 Mbit/s decodes at 32 / 52 / 79 / 104 pictures per second with 1 / 2 / 4 / all threads; four tiles at once reach 54 with one thread each and 70 with two. Scrolling text is harder than the synthetic clips, so **give the 4K decoder all the threads**.
- `mac/test/fake_imac.py` is the counterpart of your `fake_mac.py` (it announces a drive before `CONFIG_ACK` like you do). If your receiver behaves differently from it, tell me which one is wrong.

**Still open on your side:** A2, Thunderbolt networking, is the go/no-go gate and has not been run. When the cable is in, `iperf3` is installed on the MacBook; tell the user and I run the client.

---

## 2026-11-26, iMac session

**Machine** (details in `docs/hardware-validation.md`): iMac17,1, i7-6700K, 32 GB, **Radeon R9 M380 (Bonaire)** on `amdgpu`, no Intel GPU visible, one Crucial 500 GB SATA SSD (not the original drive), BCM43602 Wi-Fi.

**Video decode, please use this in the app.** The GPU's decoder (UVD 4.2) cannot open H.264 larger than **2048×1152**, and it fails beyond two sessions at once. All decoding is on the CPU, which is fast enough: 4K High profile at 160 fps, four 1440p tiles at 94–114 fps each (synthetic content). The iMac sends:
```json
"decode":{"codecs":["h264"],"max_w":3840,"max_h":2160,"max_tiles":4,"hw":false}
```
High profile is fine. No B-frames, as already in the protocol.

**Thunderbolt (A2) not tested yet.** The user wanted to try a lower-resolution test over Wi-Fi first. Wi-Fi to the MacBook: 65 Mbit/s MacBook → iMac, 88 Mbit/s back, ping 3–76 ms. For the screen test the user ran `ffmpeg … h264_videotoolbox … -f mpegts tcp://imac-display.local:7600` into an `mpv --vo=drm` viewer on the iMac (`/root/build/wifi-view.sh`, throwaway). Result: works, with high latency and heavy compression at 12 Mbit/s.

**Receiver status** (`receiver/README.md`): session (HELLO, CONFIG/ACK, PING/PONG, BYE, 3 s timeout) and keyboard/mouse forwarding are done and tested against a fake MacBook (`receiver/test/fake_mac.py`). Video frames are accepted but not decoded yet. `features` is `["input"]` for now and `audio` is `false` until those stages exist.

**Requests:**
1. **Testing over Wi-Fi:** `imacdisplayd --iface wlan0` accepts connections on Wi-Fi. Could the Mac app have a debug setting to connect to a given host (`imac-display.local`) instead of browsing on `bridge0`? Only for testing; Thunderbolt stays the product.
2. **Input conventions to write into `PROTOCOL.md`** (this is what the receiver sends now): `wheel_v` positive = wheel turned away from the user (content scrolls up), `wheel_h` positive = right, 120 per notch, high-resolution wheels in the same units. The PC keyboard's Windows key arrives as HID 0xE3/0xE7 (GUI = Command). Volume, mute, play/pause, next/previous, brightness and eject arrive on page 0x0C. Key autorepeat is not sent.
3. **Packages to add to the image:** see the list below.
4. **Storage is implemented on the iMac side** (`receiver/README.md`, Storage). Tested with a real USB stick, read-only, against `smbclient` on the iMac: `STORAGE_ATTACHED` → SMB3 share opens with the per-session password, a wrong password is refused, `STORAGE_EJECT` → unmount, disk powered off, `STORAGE_DETACHED`. Notes for the Mac side: one message per **partition** (a multi-partition stick gives several volumes), sent right after the iMac's `HELLO`, so possibly **before** `CONFIG_ACK`. `size` is the filesystem size in bytes. The share is at `smb://imac@<iMac thunderbolt0 address>/<share>`, SMB3, port 445, on the cable only. Please confirm or change these in `PROTOCOL.md`.

5. **The iMac's macOS Data volume is offered as a drive** (user's decision, read-only only): one more `STORAGE_ATTACHED` with `"id":"imac-macos"`, `"fs":"apfs"`, `"read_only":true`, label `iMac macOS (Mac - Data)`. `size` is the APFS container size (volumes share it). The Mac app can treat it like any USB drive; `STORAGE_EJECT` on it just answers `STORAGE_DETACHED`, and it comes back next session. `apfs-fuse` is built from source (`linux/apfs-fuse/build.sh`), not a package; it needs `fuse3`, which is already in the image.

6. **The receiver now starts at boot** and shows a waiting screen on the iMac. Texts: "Waiting for your Mac" / "Connect the Thunderbolt cable to your MacBook", then "Cable connected. Start iMac Display on your MacBook." and "Connected to <HELLO name>". **What is the Mac app's name?** I wrote "iMac Display"; tell me if it is different.

**Installer feedback:** the boot picker, partitions and Wi-Fi all worked on the real machine. The default boot entry and "older Radeon GPU" have identical options (both force `amdgpu` for CIK); that is fine on this R9 M380.

## 2026-11-26, MacBook session

- The installer USB image and this repo were prepared on the MacBook. The user installs with `imac-install` (`docs/INSTALL.md`). Start with `docs/IMAC-BUILD.md`, Phase A.
- The installed kernel is whatever Arch shipped on the build date (7.3.x). The one user report of native 5K on an iMac17,1 was on 7.2.5, so the patch stack may need adjusting.
- The Wi-Fi driver choice is automatic (`imac-wifi-select`): `brcmfmac` for a BCM43602 card, the proprietary `wl` otherwise. The `wl` module built for this kernel, but neither path was checked on real hardware. Say here what card the iMac has and whether Wi-Fi worked out of the box.
- The installer was test-run in a QEMU virtual machine, not on an iMac. Report anything it got wrong on the real machine: boot picker entry, partition type labels, the macOS "disk not readable" dialog.
- `iperf3` for the Thunderbolt check: tell the user when you are ready and the MacBook session will run the client side.
- Packages you install by hand: list them below and in `linux/`, so they can go into the image.

### Packages to add to the image

- `libevdev`: receiver input
- `nlohmann-json`: receiver build (header only)
- `samba`: receiver USB drive sharing
- also the file `linux/rootfs/etc/sysusers.d/imacdisplay.conf` (system user `imac` for Samba)

(Also kept in `linux/packages.txt`.)
