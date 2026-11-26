# imacdisplayd

The iMac side of `PROTOCOL.md`. Status, in the build order of `docs/IMAC-BUILD.md` Phase C:

| Stage | State |
|---|---|
| 1. Waiting screen | not started |
| 2. Session: HELLO, CONFIG, CONFIG_ACK, PING/PONG, BYE, 3 s timeout | done, tested with `test/fake_mac.py` |
| 3. Video | video channel accepted, frames counted and dropped; no decode yet |
| 4. Sound | not started |
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

## Input

- Only USB and Bluetooth devices are used (not the iMac's own power button and other ACPI events).
- Devices are grabbed only while a MacBook session is active. Without a session the keyboard
  works on the iMac console as usual; when the session ends or the receiver exits, they are released.
- Keys go out as USB HID usages: page 0x07 for keyboard keys, page 0x0C for volume, mute, media
  and brightness keys. Key repeat is not forwarded (the MacBook repeats).
- Pointer: relative counts, coalesced per evdev report. Wheel: 120 per notch (high-resolution wheels
  pass their own units), positive `wheel_v` = scroll away from the user (up), positive `wheel_h` = right.
  The Windows/Super key is HID 0xE3/0xE7 (GUI), which macOS treats as Command.

## Storage

- Only filesystems on USB disks are used (`ID_BUS=usb`). The disk holding `/` and `/boot` is refused again
  explicitly, so the internal drive can never be mounted, whatever udev reports.
- Each partition with a filesystem is one volume. EFI and BIOS-boot partitions are skipped, and so is a whole
  disk that has a partition table (hybrid ISO sticks report `iso9660` on the disk and on the partition).
- Mounted at `/run/imacdisplay/volumes/<id>` with `nosuid,nodev,noexec,noatime`. Read-write: FAT, exFAT,
  NTFS (`ntfs3`), ext2/3/4, btrfs, XFS, F2FS. Always read-only: ISO 9660, UDF, HFS/HFS+. If a read-write
  mount is refused (write-protect switch, dirty NTFS), it falls back to read-only.
- Drives are mounted as soon as they are plugged in. They are shared only while a MacBook session is active:
  `smbd` runs as a child of the receiver with a generated config (`/run/imacdisplay/samba/smb.conf`), bound to
  the session interface only, SMB3 minimum, one user `imac` with a new random password each session.
  Files are written as root (`force user = root`). Mac metadata goes into extended attributes where the
  filesystem has them (`fruit` + `streams_xattr`); on FAT/exFAT/NTFS macOS writes its own `._` files.
- `STORAGE_EJECT` unmounts, then powers the disk off (`/sys/block/sdX/device/delete`) once none of its
  partitions is still mounted, then answers `STORAGE_DETACHED`. A pulled drive is lazily unmounted and
  reported as `STORAGE_DETACHED` too. After an eject the drive has to be replugged to appear again.
- Known gap: if the receiver is killed with SIGKILL, `smbd` keeps running and the drives stay mounted;
  the next start of the receiver does not clean them up yet.

## Test without the MacBook

    imacdisplayd --iface lo --port 7501 &
    test/fake_mac.py ::1 --port 7501 --seconds 10     # prints what the MacBook would receive
    test/uinput_inject.py                             # in another shell: virtual keyboard+mouse
    test/fake_mac.py ::1 --port 7501 --eject-after 10 # also ejects each announced drive after 10 s
