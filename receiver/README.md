# imacdisplayd

The iMac side of `PROTOCOL.md`. Status, in the build order of `docs/IMAC-BUILD.md` Phase C:

| Stage | State |
|---|---|
| 1. Waiting screen | done: laptop + Thunderbolt symbol, cable/session state, animated dots, footer with link address (`display.cpp`, DRM dumb buffer + cairo) |
| 2. Session: HELLO, CONFIG, CONFIG_ACK, PING/PONG, BYE, 3 s timeout | done, tested with `test/fake_mac.py` |
| 3. Video | done: CPU decode per tile, GPU display, tiles may be left out, `VIDEO_STATS`, keyframe requests (`video.cpp`) |
| 4. Sound | done: `AUDIO_OUT` playback, `AUDIO_IN` microphone, `AUDIO_VOLUME` (`audio.cpp`); not yet checked by ear |
| 5. Input: keyboards and pointers → `INPUT_*` | done, tested with a virtual USB device (`test/uinput_inject.py`) |
| 6. Storage: USB drives → SMB shares, `STORAGE_*` | done, tested read-only with a real USB stick and `smbclient` |

## Build

    cmake -S receiver -B /root/build/receiver && make -C /root/build/receiver -j8

Needs `libevdev`, `libudev` (systemd), `nlohmann-json`, `cmake`, a C++17 compiler. At run time: `samba` and the
system user `imac` (`linux/rootfs/etc/sysusers.d/imacdisplay.conf`, created with `systemd-sysusers`).

## Run

    /root/build/receiver/imacdisplayd                  # the real thing: thunderbolt0, port 7500
    /root/build/receiver/imacdisplayd --iface wlan0    # testing only: accept over Wi-Fi
    ... --storage-read-only                            # testing: mount every USB drive read-only

It retries every second until the interface exists, so it can start before the cable is plugged in.

## Discovery

`_imacdisplay._tcp` (port 7500, TXT `proto=0 model=iMac17,1 panel=5120x2880`) is published by
`systemd-resolved` from `/etc/systemd/dnssd/imacdisplay.dnssd` on every link with mDNS on (Thunderbolt
and Wi-Fi). No Avahi: one mDNS responder only.

## At boot

Installed as a systemd service (files under `linux/rootfs/`, copied to `/`):

- `/usr/local/bin/imacdisplayd`, `/etc/systemd/system/imacdisplayd.service` (enabled, restarts after 2 s
  if it stops), optional arguments in `/etc/imacdisplayd.conf`.
- It owns tty1 and switches it to graphics mode. The auto-login root console on tty1 is off while
  the receiver runs (`getty@tty1.service.d/receiver.conf`), so typing on the iMac never reaches a hidden shell.
- **Way back in:** SSH always works. In the boot menu, **"console only (receiver off)"**
  (`imacdisplay.noreceiver`) or **"safe graphics"** (`nomodeset`) start the old auto-login console instead.
- The screen waits for the real GPU driver (`amdgpu`) and ignores the firmware framebuffer (`simpledrm`)
  that exists for the first seconds of boot.

    journalctl -u imacdisplayd -f        # what it is doing
    systemctl restart imacdisplayd

## Input

- Only USB and Bluetooth devices are used (not the iMac's own power button and other ACPI events).
- Devices are grabbed only while a MacBook session is active. Without a session the keyboard
  works on the iMac console as usual; when the session ends or the receiver exits, they are released.
- Keys go out as USB HID usages: page 0x07 for keyboard keys, page 0x0C for volume, mute, media
  and brightness keys. Key repeat is not forwarded (the MacBook repeats).
- Pointer: relative counts, coalesced per evdev report. Wheel: 120 per notch (high-resolution wheels
  pass their own units), positive `wheel_v` = scroll away from the user (up), positive `wheel_h` = right.
  The Windows/Super key is HID 0xE3/0xE7 (GUI), which macOS treats as Command.

## Video

- One decoder thread per tile (libavcodec, `AV_CODEC_FLAG_LOW_DELAY`, 2 slice threads). Only a single tile
  larger than 2560×1440 gets frame threading (3 threads, about 2 frames of extra delay).
- Each decoded tile is copied by its decoder thread into a persistently mapped pixel buffer (5 picture
  slots); the presenter (GLES 3 on GBM/EGL) uploads from there on the GPU, converts YUV to RGB
  (BT.601/709, video/full range from the stream), fits the picture to the panel and page-flips.
- A picture is complete when the packet flagged "last tile" has arrived and every tile sent for it is
  decoded. Tiles left out keep their last picture. A newer complete picture replaces one not shown yet;
  the replaced one's tiles are merged in, so no tile update is lost.
- Decoders never wait: a picture that cannot be kept is dropped, packets are not (dropping packets costs
  a keyframe). A tile's backlog is dropped only when its oldest packet is older than 500 ms; then a
  keyframe is requested. Keyframe requests: at most one per 250 ms, a held-back one is sent when the
  250 ms are over, repeated every second while a decoder waits.
- During a session the CPU's energy preference is set to `performance` (restored afterwards): with the
  default, bursts of decoding run at a low clock (1080p: 12 ms instead of 4.2 ms per frame at 30 fps).
- Open: the page flip takes a full refresh (~16 ms) in steady state at 60 fps; receive-to-screen is
  17–30 ms on loopback.

## Sound

- `AUDIO_OUT` goes into a jitter buffer; a playback thread writes 5 ms periods to `plughw:PCH,0` (the
  built-in CS4206: speakers, or the headphone jack when something is plugged in). Playback starts at 30 ms
  buffered; on underrun silence is played and it refills to 30 ms; above 60 ms, 10 ms are dropped (the
  Mac's clock running slightly fast).
- `AUDIO_IN`: a capture thread sends 10 ms messages when `CONFIG.audio_in.enabled`.
- `AUDIO_VOLUME` sets the codec's Master control on a perceptual curve (1 = 0 dB, 0.5 = −18 dB,
  0.25 = −36 dB, 0 = muted). Without it, the mixer levels are left as they are.

## Storage

- Only filesystems on USB disks are used (`ID_BUS=usb`). The disk holding `/` and `/boot` is refused again
  explicitly, so the internal drive can never be mounted, whatever udev reports.
- Each partition with a filesystem is one volume. EFI and BIOS-boot partitions are skipped, and so is a whole
  disk that has a partition table (hybrid ISO sticks report `iso9660` on the disk and on the partition).
- Mounted at `/run/imacdisplay/volumes/<id>` with `nosuid,nodev,noexec,noatime`. Read-write: FAT, exFAT,
  NTFS (`ntfs3`), ext2/3/4, btrfs, XFS, F2FS. Always read-only: ISO 9660, UDF, HFS/HFS+. If a read-write
  mount is refused (write-protect switch, dirty NTFS), it falls back to read-only.
- The SMB share name is made from the label (Finder shows it as the volume name): letters, digits, space
  and `._-()` kept, other characters become `_`, made unique with " 2", " 3". It can contain spaces, so
  the Mac percent-encodes it in the `smb://` URL.
- Drives are mounted as soon as they are plugged in. They are shared only while a MacBook session is active:
  `smbd` runs as a child of the receiver with a generated config (`/run/imacdisplay/samba/smb.conf`), bound to
  the session interface only, SMB3 minimum, one user `imac` with a new random password each session.
  Files are written as root (`force user = root`). Mac metadata goes into extended attributes where the
  filesystem has them (`fruit` + `streams_xattr`); on FAT/exFAT/NTFS macOS writes its own `._` files.
- `STORAGE_EJECT` unmounts, then powers the disk off (`/sys/block/sdX/device/delete`) once none of its
  partitions is still mounted, then answers `STORAGE_DETACHED`. A pulled drive is lazily unmounted and
  reported as `STORAGE_DETACHED` too. A drive that was ejected but not unplugged is switched back on (its USB
  device de- and re-authorized) at the start of the next session, so it appears again.
- **The iMac's own macOS** (on by default, `--no-macos-share` turns it off): the APFS **Data** volume on the
  internal disk is mounted **read-only** with `apfs-fuse` (built by `linux/apfs-fuse/build.sh`; it opens the
  disk `O_RDONLY` and has no write operations) at `/run/imacdisplay/volumes/imac-macos` and offered as share
  `imac-macos`, always `read_only: true`. FileVault-encrypted volumes are skipped (no passphrase support yet).
  Eject from the MacBook only answers `STORAGE_DETACHED`; the internal disk is never unmounted mid-run or
  powered off, and it is offered again in the next session. Read/write was considered and rejected by the
  user on 2026-11-26: Linux APFS write support is experimental and the project never writes outside the
  Linux partitions.
- `smbd` runs in its own process group: when it shuts down it signals its whole group, which once ended
  the receiver at every session end.
- Known gap: if the receiver is killed with SIGKILL, `smbd` keeps running and the drives stay mounted;
  the next start of the receiver does not clean them up yet.

## Test without the MacBook

    imacdisplayd --iface lo --port 7501 &
    test/fake_mac.py ::1 --port 7501 --seconds 10     # prints what the MacBook would receive
    test/uinput_inject.py                             # in another shell: virtual keyboard+mouse
    test/fake_mac.py ::1 --port 7501 --eject-after 10 # also ejects each announced drive after 10 s
    test/send_video.py ::1 --port 7501 --size 3840x2160 --tiles 4 [--partial] [--clip FILE]
    test/send_audio.py ::1 --port 7501 [--tone 440 --level -30]   # silence unless --tone
