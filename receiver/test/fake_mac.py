#!/usr/bin/env python3
"""Pretends to be the MacBook app, for testing imacdisplayd without the Mac.

Connects the ctrl channel, does HELLO and CONFIG, answers PING, and prints every
message the iMac sends (input events decoded). Runs until Ctrl-C or --seconds.

    fake_mac.py HOST [--port 7500] [--seconds N] [--video]
"""
import argparse, json, os, socket, struct, sys, time, uuid, select

HDR = struct.Struct("<IBBHQ")
NAMES = {0x01: "HELLO", 0x02: "CONFIG", 0x03: "CONFIG_ACK", 0x04: "PING", 0x05: "PONG", 0x06: "BYE",
         0x11: "VIDEO_REQUEST_KEYFRAME", 0x12: "VIDEO_STATS", 0x21: "AUDIO_IN",
         0x30: "INPUT_KEY", 0x31: "INPUT_POINTER_REL", 0x32: "INPUT_BUTTON",
         0x40: "STORAGE_ATTACHED", 0x42: "STORAGE_DETACHED"}


def now_us():
    return time.monotonic_ns() // 1000


def frame(t, payload=b"", flags=0, tile=0):
    return HDR.pack(len(payload), t, flags, tile, now_us()) + payload


def jframe(t, obj):
    return frame(t, json.dumps(obj).encode())


class Reader:
    def __init__(self, sock):
        self.s, self.buf = sock, b""

    def frames(self):
        data = self.s.recv(1 << 20)
        if not data:
            raise EOFError
        self.buf += data
        while len(self.buf) >= 16:
            n, t, fl, tile, pts = HDR.unpack_from(self.buf)
            if len(self.buf) < 16 + n:
                break
            yield t, fl, self.buf[16:16 + n]
            self.buf = self.buf[16 + n:]


def describe(t, p):
    if t == 0x30:
        page, usage, down = struct.unpack_from("<HHB", p)
        return f"page 0x{page:02x} usage 0x{usage:02x} {'down' if down else 'up'}"
    if t == 0x31:
        return "dx %d dy %d wheel_v %d wheel_h %d" % struct.unpack("<iiii", p)
    if t == 0x32:
        return f"button {p[0]} {'down' if p[1] else 'up'}"
    if t in (0x04, 0x05):
        return p.hex()
    try:
        return p.decode()
    except UnicodeDecodeError:
        return p.hex()


def connect(host, port):
    for fam, _, _, _, addr in socket.getaddrinfo(host, port, type=socket.SOCK_STREAM):
        s = socket.socket(fam, socket.SOCK_STREAM)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            s.connect(addr)
            return s
        except OSError:
            s.close()
    sys.exit(f"cannot connect to {host}:{port}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=7500)
    ap.add_argument("--seconds", type=float, default=0)
    ap.add_argument("--video", action="store_true", help="also open the video channel and send dummy frames")
    a = ap.parse_args()

    session = str(uuid.uuid4())
    ctrl = connect(a.host, a.port)
    ctrl.sendall(jframe(0x01, {"proto": 0, "role": "mac", "channel": "ctrl", "session": session, "name": "fake_mac.py"}))
    ctrl.sendall(jframe(0x02, {"video": {"codec": "h264", "fps": 60, "w": 1920, "h": 1080,
                                         "tiles": [{"id": 0, "x": 0, "y": 0, "w": 1920, "h": 1080}], "scale": "fit"},
                               "audio_out": {"rate": 48000, "ch": 2, "fmt": "s16le"},
                               "audio_in": {"enabled": False}}))
    video = None
    if a.video:
        video = connect(a.host, a.port)
        video.sendall(jframe(0x01, {"proto": 0, "role": "mac", "channel": "video", "session": session}))
    rd = Reader(ctrl)
    vrd = Reader(video) if video else None
    end = time.monotonic() + a.seconds if a.seconds else None
    next_ping = time.monotonic()
    try:
        while not end or time.monotonic() < end:
            now = time.monotonic()
            if now >= next_ping:
                ctrl.sendall(frame(0x04, struct.pack("<Q", now_us())))
                if video:
                    video.sendall(frame(0x10, b"\x00\x00\x00\x01" + os.urandom(1000), flags=0b10))
                next_ping = now + 1
            socks = [ctrl] + ([video] if video else [])
            r, _, _ = select.select(socks, [], [], 0.1)
            for s in r:
                for t, fl, p in (rd if s is ctrl else vrd).frames():
                    if t == 0x04:
                        ctrl.sendall(frame(0x05, p))
                    if t in (0x04, 0x05):
                        continue
                    print(f"{NAMES.get(t, hex(t))}: {describe(t, p)}", flush=True)
                    if t == 0x06:
                        return
        ctrl.sendall(jframe(0x06, {"reason": "fake_mac.py done"}))
    except (EOFError, ConnectionError):
        print("connection closed by the iMac")
    except KeyboardInterrupt:
        ctrl.sendall(jframe(0x06, {"reason": "fake_mac.py interrupted"}))


if __name__ == "__main__":
    main()
