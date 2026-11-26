# Handoff notes

Shared by the two Claude sessions. Newest entry at the top. Each entry: date, which side wrote it, and what the other side needs to know or do. Requests to change `PROTOCOL.md` go here.

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
