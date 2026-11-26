# Building the iMac side

Instructions for the Claude session running on the iMac. Read `CLAUDE.md` in the repo root first: it has the goal, the fixed decisions and who owns which files.

Work through the phases in order. Phase A decides whether the project continues at all.

## Where you are

- You are on **Arch Linux installed on the iMac's internal drive**, next to macOS. The user installed it from a USB stick with `imac-install` (see `docs/INSTALL.md`; the script is `linux/iso/airootfs/usr/local/bin/imac-install`, read it to know exactly what the system looks like).
- Linux owns two partitions: `LINUX` (ext4, mounted at `/`) and `LINUXBOOT` (FAT32, mounted at `/boot`, holds systemd-boot, the kernel and the initramfs). Everything else on the internal drive belongs to macOS.
- The system is a copy of Arch's install medium plus our tools: minimal, text console, root logs in automatically on the console, SSH accepts only the MacBook's key. This repo is at `/root/imac-display`.
- macOS is still the default system. Linux starts only when the user holds Option at power-on and chooses "EFI Boot".
- The boot menu (systemd-boot) has three entries: normal, "older Radeon GPU" (forces `amdgpu` on R9 M380/M390) and "safe graphics" (`nomodeset`).
- The user sits at the iMac or is logged in from the MacBook with `ssh root@imac-display.local`. Some checks need their eyes, ears or hands (is there a picture, is there sound, plug the cable). Ask plainly, one thing at a time.
- The MacBook session can run commands on the MacBook side (for example `iperf3` or `ifconfig bridge0`). Ask the user to relay, or leave a request in `docs/handoff.md`.

## Hard rules

1. **Stay inside the two Linux partitions.** Never write to, format, resize, or mount read-write any other partition on the internal drive. Never run partitioning tools (`sgdisk`, `sfdisk`, `parted`, `fdisk`, `gdisk`) on it in a way that writes. macOS and the user's data live there.
2. **Do not change the firmware's boot settings** (`efibootmgr`, `bless`, NVRAM). Making Linux the default system is the user's decision, later.
3. **The display link is Thunderbolt only.** Do not design around Ethernet or Wi-Fi for it. Wi-Fi is for downloads.
4. **Read third-party code before you build or load it**, kernel patches most of all.
5. **Keep a way back in.** Before changing the kernel, the graphics driver or boot parameters, make sure a known-good boot entry and kernel stay available, and tell the user which entry that is.
6. **Do not edit `PROTOCOL.md`.** Put change requests in `docs/handoff.md`.
7. Record measurements in `docs/hardware-validation.md` as you go, with the commands that produced them. Commit after each check. Do not push anywhere.

Every package you install and every system file you change is also recorded under `linux/` (package list, config files, scripts), so the system can be rebuilt from the repo.

---

## Phase A. Hardware gate (M0)

Run `m0-check` first. It writes a facts file into `docs/`. Read it, then work through the checks. Fill in `docs/hardware-validation.md`.

### A1. Identify the machine

From the facts file:

- **GPU.** `lspci -nn | grep -i vga`. Tonga = R9 M395 / M395X. Curacao or Pitcairn = R9 M390. Bonaire = R9 M380. Note which kernel driver is bound (`amdgpu` or `radeon`). If it is `radeon`, reboot with the boot-menu entry "older Radeon GPU", which switches to `amdgpu`.
- **Intel graphics.** Is an Intel HD 530 visible in `lspci`? It would give a second hardware decoder.
- **Wi-Fi card** and which driver is bound. `imac-wifi-select` (a boot service) chooses between `brcmfmac` and the proprietary `wl`.
- **Internal drive(s):** SSD, hard disk, or both (Fusion Drive), and which one Linux is on. Look only.

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
4. Kernel parameters, one at a time (press `e` on the boot-menu entry to try one without saving it): `acpi_osi=Darwin`, then `pcie_ports=native`.
5. Another kernel: `linux-lts` installed alongside, and an older or newer mainline kernel if the changelog of `drivers/thunderbolt` suggests a regression or fix for software-connection-manager XDomain on Falcon Ridge.
6. Read the kernel source for what is failing (`drivers/thunderbolt/xdomain.c`, `tb.c`) before concluding anything.

There is no fallback link. If the ladder is exhausted, write up exactly what was tried and what the logs show, tell the user, and stop.

### A3. The 5K panel

- What resolution is the console running at? `cat /sys/class/drm/card*-*/modes`, `cat /sys/class/graphics/fb0/virtual_size`, and ask the user what the picture looks like (sharp, stretched, half the screen, black).
- The panel is fed by two internal DisplayPort links as two tiles. The stock driver is not expected to drive it at 5120×2880.
- Native 5K needs community patches to `amdgpu`. Starting points: `MarkPronkin/imac5k-universal-linux-patcher` on GitHub (its issue #9 reports this exact model working with an R9 M395X on kernel 7.2.5) and `BR1UHNz/retina-5k-imac-linux` for background on the tile stitching. Neither is verified by us. Check which kernel versions the patches apply to; the installed kernel is newer than 7.2.5.
- Read the patches before building them. Build under `/root/build`.
- A broken graphics driver means a black screen. Before loading a patched `amdgpu`, confirm the user can still get in: SSH over Wi-Fi works, and the "safe graphics" boot entry starts the unpatched kernel path.
- Also note whether a backlight control exists (`ls /sys/class/backlight`).

Pass: a usable picture now, and native 5120×2880 working or a concrete path to it. If native 5K turns out to be impossible on this GPU, record the best resolution available and let the user decide.

### A4. Video decode speed

The MacBook will send H.264 at 60 fps: first one 3840×2160 stream, later four 2560×1440 streams decoded in parallel. Measure what this iMac can decode.

```sh
mkdir -p /root/build
vainfo --display drm --device /dev/dri/renderD128      # which codecs the GPU decodes
ffmpeg -f lavfi -i testsrc2=size=3840x2160:rate=60 -t 10 -c:v libx264 -preset ultrafast \
       -tune zerolatency -bf 0 -b:v 60M -pix_fmt yuv420p /root/build/t4k.mp4
ffmpeg -benchmark -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 -i /root/build/t4k.mp4 -f null -   # GPU
ffmpeg -benchmark -threads 1 -i /root/build/t4k.mp4 -f null -                                            # one CPU core
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

### A6. Report

Fill in the summary table at the top of `docs/hardware-validation.md` (pass, partial, fail, not tested, for each check), commit, and tell the user the result in plain words: go or no-go, and what each "partial" means for them.

---

## Phase B. Turn the install into an appliance (M2)

Only after Phase A passes and the user says to go on. The base system is already installed; this phase makes it boot straight into the receiver.

1. **Pin the kernel** once the panel works: `IgnorePkg = linux linux-headers` in `/etc/pacman.conf`. Set the 5K patch up so it is rebuilt if the kernel is ever changed on purpose (DKMS or a pacman hook).
2. **Avahi**, restricted to `thunderbolt0`, advertising `_imacdisplay._tcp` as described in `PROTOCOL.md`. The base system answers mDNS through `systemd-resolved`; the two must not both claim port 5353, so pick one arrangement and record it.
3. **A systemd service that starts the receiver on the bare screen at boot.** The console's automatic root login goes away once the receiver owns the screen; SSH stays.
4. **Boot time.** Target: boot-menu to the waiting screen in under 15 seconds on an SSD. Measure with `systemd-analyze`; trim what is not needed.
5. **Housekeeping:** remove packages that only the install medium needed, set the time zone, keep `systemd-timesyncd` running.

Keep every configuration file and script under `linux/` in the repo so the setup can be repeated on a fresh install.

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
