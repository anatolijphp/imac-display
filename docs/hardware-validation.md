# Hardware validation (M0)

Filled in by the iMac session while working through Phase A of `docs/IMAC-BUILD.md`. Every result names the command that produced it. Raw facts from `m0-check` are saved next to this file as `m0-facts-<timestamp>.txt`.

## Summary

Status is one of: pass, partial, fail, not tested.

| Check | Status | One-line result |
|---|---|---|
| A2 Thunderbolt networking | not tested | |
| A3 5K panel | not tested | |
| A4 Video decode | not tested | |
| A5 Sound | not tested | |
| A6 Internal drive | not tested | |

**Decision:** (go / no-go, date, and what any "partial" means for the user)

## A1. Machine

- Model and firmware:
- CPU and memory:
- GPU (chip, marketing name, driver bound):
- Intel graphics visible:
- Wi-Fi card and driver:
- Kernel and boot-menu entry used:

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

| Case | Decoder | Frames per second |
|---|---|---|
| 1 × 3840×2160 H.264 | GPU (VAAPI) | |
| 1 × 3840×2160 H.264 | CPU, 1 thread | |
| 4 × 2560×1440 H.264 at once | GPU (VAAPI) | |
| 4 × 2560×1440 H.264 at once | CPU, 1 thread each | |

- `vainfo` profiles:
- Recommended `decode` block for `HELLO`:

## A5. Sound

- Cards and codec:
- Speakers:
- Headphone jack:
- Microphone:
- Settings or quirks needed:

## A6. Internal drive

- Drives and type (SSD, hard disk, Fusion):
- Partition layout:
- Free space reported by macOS:

## Other findings

