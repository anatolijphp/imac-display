# imacdisplayd

The iMac side of `PROTOCOL.md`. Status, in the build order of `docs/IMAC-BUILD.md` Phase C:

| Stage | State |
|---|---|
| 1. Waiting screen | not started |
| 2. Session: HELLO, CONFIG, CONFIG_ACK, PING/PONG, BYE, 3 s timeout | done, tested with `test/fake_mac.py` |
| 3. Video | video channel accepted, frames counted and dropped; no decode yet |
| 4. Sound | not started |
| 5. Input: keyboards and pointers → `INPUT_*` | done, tested with a virtual USB device (`test/uinput_inject.py`) |
| 6. Storage | not started |

## Build

    cmake -S receiver -B /root/build/receiver && make -C /root/build/receiver -j8

Needs `libevdev`, `nlohmann-json`, `cmake`, a C++17 compiler.

## Run

    /root/build/receiver/imacdisplayd                  # the real thing: thunderbolt0, port 7500
    /root/build/receiver/imacdisplayd --iface wlan0    # testing only: accept over Wi-Fi

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

## Test without the MacBook

    imacdisplayd --iface lo --port 7501 &
    test/fake_mac.py ::1 --port 7501 --seconds 10     # prints what the MacBook would receive
    test/uinput_inject.py                             # in another shell: virtual keyboard+mouse
