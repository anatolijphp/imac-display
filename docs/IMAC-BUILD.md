# Building the iMac side

Instructions for the Claude session running on the iMac. Read `CLAUDE.md` in the repo root first: it has the goal, the fixed decisions and who owns which files.

Work through the phases in order. Phase A decides whether the project continues at all.

## Where you are

- You are on a **live system booted from a USB stick**. The root filesystem lives in RAM: anything you install with `pacman` or write outside `/persist` is gone after a reboot.
- `/persist` is the stick's data partition and survives reboots. This repo is at `/persist/imac-display`. Your Claude login, SSH host keys and known Wi-Fi networks are kept there too.
- If you need a package that is not in the image, install it with `pacman -Sy <name>` and list it in `docs/handoff.md` so the MacBook session can add it to the image.
- The user sits at the iMac or is logged in from the MacBook with `ssh root@imac-live.local`. Some checks need their eyes, ears or hands (is there a picture, is there sound, plug the cable). Ask plainly, one thing at a time.
- The MacBook session can run commands on the MacBook side (for example `iperf3` or `ifconfig bridge0`). Ask the user to relay, or leave a request in `docs/handoff.md`.

## Hard rules

1. **Do not modify the iMac's internal drive.** No partitioning, no formatting, no read-write mounts, until the user has confirmed a Time Machine backup and approved a written partition plan (Phase B). Inspecting it read-only is fine.
2. **The display link is Thunderbolt only.** Do not design around Ethernet or Wi-Fi for it. Wi-Fi is for downloads.
3. **Read third-party code before you build or load it**, kernel patches most of all.
4. **Do not edit `PROTOCOL.md`.** Put change requests in `docs/handoff.md`.
5. Record measurements in `docs/hardware-validation.md` as you go, with the commands that produced them. Commit after each check. Do not push anywhere.

---

## Phase A. Hardware gate (M0)

Run `m0-check` first. It writes a facts file into `docs/`. Read it, then work through the checks. Fill in `docs/hardware-validation.md`.

### A1. Identify the machine

From the facts file:

- **GPU.** `lspci -nn | grep -i vga`. Tonga = R9 M395 / M395X. Curacao or Pitcairn = R9 M390. Bonaire = R9 M380. Note which kernel driver is bound (`amdgpu` or `radeon`). If it is `radeon`, reboot with the boot-menu entry "older Radeon GPU", which switches to `amdgpu`.
- **Intel graphics.** Is an Intel HD 530 visible in `lspci`? It would give a second hardware decoder.
- **Wi-Fi card** and which driver is bound.
- **Internal drive(s):** SSD, hard disk, or both (Fusion Drive).

### A2. Thunderbolt networking (the go/no-go check)

Ask the user to connect the cable: MacBook USB-C port → Apple Thunderbolt 3–to–2 adapter → Thunderbolt 2 cable → either Thunderbolt port on the iMac.

```sh
lspci -nn | grep -i thunderbolt              # Falcon Ridge controller present?
lsmod | grep thunderbolt                     # thunderbolt and thunderbolt_net loaded?
ls /sys/bus/thunderbolt/devices/             # expect a domain (0-0) and an entry for the MacBook
dmesg | grep -iE 'thunderbolt|xdomain'
ip -br link; ip -br addr                     # expect thunderbolt0 with a 169.254.x.x and an fe80:: address
ping -c3 -I thunderbolt0 ff02::1             # the MacBook should answer on its link-local address
```

On the MacBook the link shows as "Thunderbolt Bridge" (`ifconfig bridge0`, status active, a 169.254.x.x address).

Pass conditions, all of them:

- `thunderbolt0` appears and both sides can ping each other.
- `iperf3` reaches at least 2 Gbit/s (`iperf3 -s` here; the MacBook runs `iperf3 -c <iMac address>` and again with `-R`).
- The link comes back after unplugging and replugging the cable, three times in a row.
- It works when the cable is already connected at power-on, and when it is connected after boot.

If the link does not come up, work down this list and record each attempt:

1. Boot with the cable already connected. Apple firmware can leave the Thunderbolt controller powered down when nothing is attached at boot.
2. Confirm the MacBook sees the iMac: System Information → Thunderbolt on the MacBook.
3. `modprobe thunderbolt_net`; turn on driver debug output: `echo 'module thunderbolt +p' > /sys/kernel/debug/dynamic_debug/control`, replug, read `dmesg`.
4. Kernel parameters, one at a time, from the boot menu (press `e` on the entry): `acpi_osi=Darwin`, then `pcie_ports=native`.
5. Another kernel: `linux-lts`, and an older or newer mainline kernel if the changelog of `drivers/thunderbolt` suggests a regression or fix for software-connection-manager XDomain on Falcon Ridge.
6. Read the kernel source for what is failing (`drivers/thunderbolt/xdomain.c`, `tb.c`) before concluding anything.

There is no fallback link. If the ladder is exhausted, write up exactly what was tried and what the logs show, tell the user, and stop.

### A3. The 5K panel

- What resolution is the console running at? `cat /sys/class/drm/card*-*/modes`, `cat /sys/class/graphics/fb0/virtual_size`, and ask the user what the picture looks like (sharp, stretched, half the screen, black).
- The panel is fed by two internal DisplayPort links as two tiles. The stock driver is not expected to drive it at 5120×2880.
- Native 5K needs community patches to `amdgpu`. Starting points: `MarkPronkin/imac5k-universal-linux-patcher` on GitHub (its issue #9 reports this exact model working with an R9 M395X on kernel 7.2.5) and `BR1UHNz/retina-5k-imac-linux` for background on the tile stitching. Neither is verified by us. Check which kernel versions the patches apply to; the image ships a newer kernel than 7.2.5.
- Read the patches before building them. Build under `/persist/build`, not in RAM.
- If trying the patch in the live session is not practical, record "native 5K: not yet tested" and what it will need; it is then the first job after the install in Phase B.
- Also note whether a backlight control exists (`ls /sys/class/backlight`).

Pass: a usable picture now, and a concrete path to 5120×2880. If native 5K turns out to be impossible on this GPU, record the best resolution available and let the user decide.

### A4. Video decode speed

The MacBook will send H.264 at 60 fps: first one 3840×2160 stream, later four 2560×1440 streams decoded in parallel. Measure what this iMac can decode.

```sh
vainfo --display drm --device /dev/dri/renderD128      # which codecs the GPU decodes
ffmpeg -f lavfi -i testsrc2=size=3840x2160:rate=60 -t 10 -c:v libx264 -preset ultrafast \
       -tune zerolatency -bf 0 -b:v 60M -pix_fmt yuv420p /persist/build/t4k.mp4
ffmpeg -benchmark -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 -i /persist/build/t4k.mp4 -f null -   # GPU
ffmpeg -benchmark -threads 1 -i /persist/build/t4k.mp4 -f null -                                            # one CPU core
```

Repeat with a 2560×1440 clip, running four decodes at the same time. Record frames per second for each case. Pass: at least 90 fps where 60 is needed, so there is headroom. Report what the `decode` block in `PROTOCOL.md`'s `HELLO` should say for this machine.

### A5. Sound

```sh
aplay -l; arecord -l
amixer -c0 scontrols                          # unmute and raise Master, Speaker, Headphone
speaker-test -c2 -t sine -f 440 -l 1
arecord -d 3 -f cd /tmp/mic.wav && aplay /tmp/mic.wav
```

Ask the user: sound from the built-in speakers? From headphones in the 3.5 mm jack? Does the recording play back their voice?

Silent speakers are a known problem on this family: the Cirrus CS4208 codec needs its external amplifier switched on. Try the codec quirks: `modprobe -r snd_hda_intel; modprobe snd_hda_intel model=mbp11` (then `mba6`, `macmini`, `gpio0`). A kernel patch proposed in 2026 for the iMac16,1 reuses the MacBook Pro 11 fixup, which is why `mbp11` is the first guess.

Pass: speakers, jack and microphone all work, with the settings needed written down. Jack-only is a partial pass; record it.

### A6. Internal drive (look, do not touch)

`lsblk -o NAME,SIZE,TYPE,TRAN,ROTA,FSTYPE,LABEL,MODEL`, `smartctl -i`, `sgdisk -p` or `fdisk -l`. Record the layout, whether it is a Fusion Drive, and how the disks are partitioned. Ask the user how much free space macOS reports. Linux needs about 40 GB.

### A7. Report

Fill in the summary table at the top of `docs/hardware-validation.md` (pass, partial, fail, not tested, for each check), commit, and tell the user the result in plain words: go or no-go, and what each "partial" means for them.

---

## Phase B. Install (M2)

Only after Phase A passes and the user says to go on.

1. **Write a partition plan** in `docs/handoff.md`: current layout, what will be created, where. Get the user's explicit approval and their confirmation that a Time Machine backup exists. No approval, no install.
2. **Free space is made from macOS, by the user**, not from Linux: Disk Utility, or `diskutil apfs resizeContainer`. Linux tools must never resize APFS or Fusion volumes. Give the user the exact steps.
3. In the freed space create a 512 MB EFI partition and an ext4 root. Leave Apple's own EFI partition alone.
4. Install minimal Arch (`pacstrap`): `base`, `linux`, `linux-headers`, `linux-firmware`, plus what the receiver needs. No desktop, no display manager.
5. Boot loader: systemd-boot on the new EFI partition at the fallback path (`bootctl install --no-variables`). Do not write firmware boot entries with `efibootmgr`; the user chooses the system by holding Option at power-on.
6. Pin the kernel once the panel works (`IgnorePkg = linux linux-headers`), and apply the 5K patch so that it is rebuilt if the kernel is ever changed deliberately.
7. Hostname `imac-display`. SSH with the MacBook's key (copy `/root/.ssh/authorized_keys` from the live system). The same `thunderbolt0` network configuration as the live system (`/etc/systemd/network/15-thunderbolt.network`, `/etc/modules-load.d/imac-thunderbolt.conf`).
8. Avahi, restricted to `thunderbolt0`, advertising `_imacdisplay._tcp` as described in `PROTOCOL.md`.
9. A systemd service that starts the receiver on the bare screen at boot. Target: power button to the waiting screen in under 15 seconds on an SSD.

Keep every configuration file and script under `linux/` in the repo so the install can be repeated.

---

## Phase C. The receiver, `imacdisplayd` (M3 to M6)

Lives in `receiver/`. It implements the iMac side of `PROTOCOL.md`.

Suggested stack, change it if measurements say otherwise: C++17 and CMake; FFmpeg's `libavcodec` for decoding (VAAPI and software); DRM/KMS with OpenGL for output, either directly or through SDL2's KMSDRM backend; ALSA for sound; `libevdev` for input.

Build it in this order, each stage working before the next starts:

1. **Waiting screen.** Take over the display with no desktop running and show "Waiting for MacBook".
2. **Session.** Listen on port 7500 on `thunderbolt0`, do the `HELLO` and `CONFIG` exchange, answer `PING`.
3. **Video.** Decode `VIDEO_FRAME`s per tile, in parallel, and show each picture at the next refresh after its last tile. Send `VIDEO_STATS`. Until the MacBook app exists, test with a small sender script that wraps an FFmpeg-encoded H.264 stream in protocol frames.
4. **Sound.** Play `AUDIO_OUT` through ALSA with a 20 to 40 ms buffer; capture the microphone and send `AUDIO_IN`.
5. **Input.** Read every keyboard and pointer with `libevdev` (grab them), translate to HID usages, send `INPUT_*`.
6. **Storage.** On USB drive insertion: mount it, share it with Samba on `thunderbolt0` only, send `STORAGE_ATTACHED`; unmount and power the drive down on `STORAGE_EJECT`.

Measure latency at each stage and write the numbers into `docs/handoff.md`.
