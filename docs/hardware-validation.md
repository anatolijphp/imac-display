# Hardware validation (M0)

Filled in by the iMac session while working through Phase A of `docs/IMAC-BUILD.md`. Every result names the command that produced it. Raw facts from `m0-check` are saved next to this file as `m0-facts-<timestamp>.txt`.

## Summary

Status is one of: pass, partial, fail, not tested.

| Check | Status | One-line result |
|---|---|---|
| A2 Thunderbolt networking | not tested | |
| A3 5K panel | not tested | |
| A4 Video decode | not tested | |
| A5 Sound | partial | Speakers work (stock driver). Jack and microphone not tested yet. |

**Decision:** (go / no-go, date, and what any "partial" means for the user)

## A1. Machine

- Model and firmware: iMac17,1, firmware 529.120.1.0.0 (`dmidecode`).
- CPU and memory: Intel Core i7-6700K, 4 cores / 8 threads, 4.0 GHz; 32 GB RAM (`lscpu`, `free -h`).
- GPU (chip, marketing name, driver bound): **Bonaire** (GCN 1.1 / CIK), `1002:6640`, subsystem `106b:014b` "Radeon R9 M380 Mac Edition", 2 GB GDDR5. Bound to `amdgpu` (`lspci -nnk`; dmesg: `initializing kernel modesetting (BONAIRE ...)`, `Display Core ... on DCE 8.0`, `uvd_v4_2`, `vce_v2_0`).
- Intel graphics visible: **no**. No Intel VGA device in `lspci`; the firmware hides the HD 530. No second decoder.
- Wi-Fi card and driver: Broadcom **BCM43602** `14e4:43ba`, bound to `brcmfmac` (`imac-wifi-select` picked it). Works out of the box: associated on 5 GHz, IPv4 and IPv6 addresses (`iw dev`, `ip -br addr`). Also present: Broadcom BCM57766 Gigabit Ethernet (`tg3`, not used), SD card reader, Apple Bluetooth `05ac:8294` (`btusb`), FaceTime HD camera `05ac:8511`.
- Internal drives (SSD, hard disk, Fusion) and which one Linux is on: one SATA SSD only, Crucial **CT500BX500SSD1** 500 GB (not the original Apple drive; no Fusion hard disk). Partitions: `sda1` EFI 200M, `sda2` APFS 371.5G (macOS), `sda3` LINUXBOOT 954M vfat, `sda4` LINUX 93.1G ext4. Linux is on `sda` (`lsblk`, `smartctl -i`, looked at only).
- Kernel and boot-menu entry used: `7.3.6-arch1-1`. Command line has the `amdgpu.cik_support=1 radeon.cik_support=0` options; the default entry and the "older Radeon GPU" entry have identical options, so both bind `amdgpu` (see Other findings).

## A2. Thunderbolt networking

- Controller (`lspci`):
- Interface and addresses on the iMac:
- Interface and addresses on the MacBook:
- Ping both ways:
- `iperf3` iMac → MacBook / MacBook → iMac:
- Replug three times:
- Cable connected at power-on / connected after boot:
- Kernel parameters or workarounds needed:

## A3. 5K panel

- Console resolution with the stock driver:
- Connectors and modes:
- What the picture looks like:
- Native 5120×2880 (patch tried, kernel version, result):
- Backlight control:

## A4. Video decode

Test clips (10 s, 600 frames, 60 fps) were made with libx264 and are under `/root/build`. The commands are in IMAC-BUILD A4. The **Baseline** clips (`t4k.mp4`, `t1440.mp4`) are the ones from the build doc: `-preset ultrafast`, which gives Constrained Baseline without CABAC on flat test patterns, so they are easy to decode. Harder clips (`h*.mp4`) were added so the numbers are not flattering: `testsrc2` plus `noise=alls=12:allf=t`, `-preset veryfast -tune zerolatency -profile:v high -bf 0`, constant bitrate (4K 60 Mbit/s, 1440p 30 Mbit/s, 1080p 20 Mbit/s). Speed = 600 frames divided by the wall-clock time of `ffmpeg -loglevel error ... -i CLIP -f null -`. With four decodes, all four run at the same time and the number is per stream. Script: `linux/bench/decode-bench.sh`.

| Case | Decoder | Frames per second |
|---|---|---|
| 1 × 3840×2160 H.264 | GPU (VAAPI) | **not possible**: `Hardware does not support image size 3840x2160 (constraints: width 64-2048 height 64-1152)` |
| 1 × 3840×2160 H.264 | CPU, 1 thread | 168 (Baseline clip) / **48** (High clip) |
| 1 × 3840×2160 H.264 | CPU, 2 / 4 / auto (8) threads | High clip: 84 / 132 / **160** |
| 4 × 2560×1440 H.264 at once | GPU (VAAPI) | **not possible**: same 2048×1152 limit |
| 4 × 2560×1440 H.264 at once | CPU, 1 thread each | **94** each (High clip); 1 stream alone: 98 |
| 4 × 2560×1440 H.264 at once | CPU, 2 threads each | **114** each (High clip) |
| 1 × 1920×1080 H.264 | GPU (VAAPI, frames left on GPU) | 75 |
| 4 × 1920×1080 H.264 at once | GPU (VAAPI) | 2 of 4 aborted: `amdgpu: The CS has been rejected ... (-12)`; the other 2 ran at 72 |
| 1 × 1920×1080 H.264 | CPU, 1 thread | 153 |

- `vainfo` profiles (`vainfo --display drm --device /dev/dri/renderD128`, Mesa 26.3.1 radeonsi, UVD 4.2): MPEG-2, VC-1, H.264 Constrained Baseline / Main / High decode (VLD) and encode (EncSlice). No HEVC. Mesa limits H.264 decode to **2048×1152**.
- **Result:** the GPU decoder is no use here. It cannot open 4K or 1440p, it reaches only 75 fps at 1080p, and the kernel rejects more than two decode sessions at once. The CPU (i7-6700K, 8 threads) clears the 90 fps bar in every case: 4K at 160 fps with all threads, four 1440p tiles at 94 fps each with one thread per stream and 114 with two. The four-tile margin is thin, and it was measured with nothing else running. Not measured yet: uploading CPU-decoded frames to the GPU for display (about 22 MB per 5K picture, 1.3 GB/s at 60 fps) and the CPU cost of sound and input at the same time. Synthetic content only; real desktop video from the MacBook's encoder may be easier or harder.
- Recommended `decode` block for `HELLO`:
  ```json
  "decode":{"codecs":["h264"],"max_w":3840,"max_h":2160,"max_tiles":4,"hw":false}
  ```
  The profile `High` is fine. Keep 4 tiles of 2560×1440 for 5K: smaller tiles that would fit the GPU (≤ 2048×1152, for example 3×3 tiles of 1708×960) do not help, because the GPU cannot run more than two sessions and tops out at 75 fps.

## A5. Sound

- Cards and codec: card 0 `HDA Intel PCH`, codec **Cirrus Logic CS4206** (`/proc/asound/card0/codec#0`, subsystem `106b:8200`), not the CS4208 the build doc expected. Driver fixup: none picked (`picked fixup  for codec SSID 106b:0000`). Pins: 0x0b internal speaker, 0x0a headphone jack, 0x0e mic. Card 1 is the GPU's HDMI audio (unused).
- Speakers: **work** with the stock driver, no quirk. `amixer -c0 sset Master/Speaker/Headphone 100% unmute`, then `speaker-test -D plughw:0,0 -c2 -t sine -f 440 -l 1`: the user heard it, very loud. Left/right not confirmed separately yet. Comfortable level afterwards: Master 40 % (−38 dB), Speaker 70 %.
- Headphone jack:
- Microphone:
- Settings or quirks needed:

## Other findings

### Wi-Fi test link (not the display link; for testing at low resolution only)

2026-11-26. iMac `wlan0` (BCM43602, `brcmfmac`) to the MacBook over the home Wi-Fi, 5 GHz channel 44, 40 MHz.

- `iw dev wlan0 link`: signal −70 dBm, rx/tx bitrate 162 Mbit/s.
- `ping -c20 -i0.2 fe80::…%wlan0` (iMac → MacBook): 0 % loss, rtt min/avg/max 3.2 / 10.7 / 75.8 ms.
- `iperf3 -s` on the iMac, `iperf3 -c imac-display.local -t 10` on the MacBook (went over IPv6 link-local). Numbers from the iMac's server log:
  - MacBook → iMac: **65.4 Mbit/s** (per-second values 54–78)
  - iMac → MacBook (`-R`): **88.2 Mbit/s**, 0 retransmits (per-second values 65–108)
- Good enough for one 1080p H.264 stream at 10–20 Mbit/s. Latency spikes of up to ~75 ms are to be expected.

