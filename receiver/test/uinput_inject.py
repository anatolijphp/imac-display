#!/usr/bin/env python3
"""Creates a virtual USB keyboard+mouse with /dev/uinput and plays a short script on it.

For testing imacdisplayd's input forwarding without touching a real keyboard. Types "ab",
Shift+1, moves the pointer, clicks, scrolls one notch, presses volume up.
Needs root (or access to /dev/uinput).
"""
import fcntl, os, struct, time

EV_SYN, EV_KEY, EV_REL = 0, 1, 2
SYN_REPORT = 0
REL_X, REL_Y, REL_WHEEL = 0, 1, 8
KEY_A, KEY_B, KEY_1, KEY_LEFTSHIFT, KEY_VOLUMEUP = 30, 48, 2, 42, 115
BTN_LEFT = 0x110
BUS_USB = 3

UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_RELBIT = 0x40045564, 0x40045565, 0x40045566
UI_DEV_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405c5503, 0x5501, 0x5502

EVENT = struct.Struct("llHHi")


def emit(fd, t, c, v):
    os.write(fd, EVENT.pack(0, 0, t, c, v))


def syn(fd):
    emit(fd, EV_SYN, SYN_REPORT, 0)


def tap(fd, key):
    emit(fd, EV_KEY, key, 1); syn(fd)
    emit(fd, EV_KEY, key, 0); syn(fd)


def main():
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    for ev in (EV_KEY, EV_REL):
        fcntl.ioctl(fd, UI_SET_EVBIT, ev)
    for k in (KEY_A, KEY_B, KEY_1, KEY_LEFTSHIFT, KEY_VOLUMEUP, BTN_LEFT):
        fcntl.ioctl(fd, UI_SET_KEYBIT, k)
    for r in (REL_X, REL_Y, REL_WHEEL):
        fcntl.ioctl(fd, UI_SET_RELBIT, r)
    # struct uinput_setup { struct input_id {u16 bustype, vendor, product, version}; char name[80]; u32 ff_effects_max; }
    setup = struct.pack("HHHH80sI", BUS_USB, 0x1234, 0x5678, 1, b"imacdisplay test keyboard+mouse", 0)
    fcntl.ioctl(fd, UI_DEV_SETUP, setup)
    fcntl.ioctl(fd, UI_DEV_CREATE)
    time.sleep(1.0)  # let the receiver notice the new device

    tap(fd, KEY_A)
    tap(fd, KEY_B)
    emit(fd, EV_KEY, KEY_LEFTSHIFT, 1); syn(fd)
    tap(fd, KEY_1)
    emit(fd, EV_KEY, KEY_LEFTSHIFT, 0); syn(fd)
    emit(fd, EV_REL, REL_X, 10); emit(fd, EV_REL, REL_Y, -5); syn(fd)
    emit(fd, EV_REL, REL_X, 3); syn(fd)
    emit(fd, EV_KEY, BTN_LEFT, 1); syn(fd)
    emit(fd, EV_KEY, BTN_LEFT, 0); syn(fd)
    emit(fd, EV_REL, REL_WHEEL, 1); syn(fd)
    tap(fd, KEY_VOLUMEUP)

    time.sleep(0.5)
    fcntl.ioctl(fd, UI_DEV_DESTROY)
    os.close(fd)


if __name__ == "__main__":
    main()
