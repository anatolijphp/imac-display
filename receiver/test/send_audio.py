#!/usr/bin/env python3
"""Test sender for sound: plays the MacBook's part. Sends AUDIO_OUT (48 kHz stereo s16le, 10 ms per
message, 100 per second, silence included, as the Mac does) and prints what comes back as AUDIO_IN.

    send_audio.py HOST [--port 7500] [--seconds 10] [--tone HZ] [--level DBFS] [--volume 0..1]

Default is silence. --tone plays a sine at --level (default -30 dBFS: quiet). --volume sends
AUDIO_VOLUME first. The receiver also needs a video CONFIG; one 1920x1080 tile is announced, no
video is sent.
"""
import argparse, json, math, select, socket, struct, sys, time, uuid

HDR = struct.Struct("<IBBHQ")


def now_us():
    return time.monotonic_ns() // 1000


def frame(t, payload=b""):
    return HDR.pack(len(payload), t, 0, 0, now_us()) + payload


def jframe(t, obj):
    return frame(t, json.dumps(obj).encode())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=7500)
    ap.add_argument("--seconds", type=float, default=10)
    ap.add_argument("--tone", type=float, default=0)
    ap.add_argument("--level", type=float, default=-30)
    ap.add_argument("--volume", type=float)
    a = ap.parse_args()

    s = None
    for fam, _, _, _, addr in socket.getaddrinfo(a.host, a.port, type=socket.SOCK_STREAM):
        s = socket.socket(fam, socket.SOCK_STREAM)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        try:
            s.connect(addr)
            break
        except OSError:
            s.close()
            s = None
    if not s:
        sys.exit("cannot connect")
    s.sendall(jframe(0x01, {"proto": 0, "role": "mac", "channel": "ctrl", "session": str(uuid.uuid4()),
                            "name": "send_audio.py"}))
    s.sendall(jframe(0x02, {"video": {"codec": "h264", "fps": 60, "w": 1920, "h": 1080,
                                      "tiles": [{"id": 0, "x": 0, "y": 0, "w": 1920, "h": 1080}], "scale": "fit"},
                            "audio_out": {"rate": 48000, "ch": 2, "fmt": "s16le"},
                            "audio_in": {"enabled": True, "rate": 48000, "ch": 1, "fmt": "s16le"}}))
    if a.volume is not None:
        s.sendall(jframe(0x22, {"volume": a.volume, "mute": False}))

    amp = 32767 * 10 ** (a.level / 20) if a.tone else 0
    n = 0
    t0 = time.monotonic()
    buf = b""
    mic_n, mic_sq, mic_peak, mic_bad = 0, 0.0, 0, 0
    last_print = t0
    while time.monotonic() - t0 < a.seconds:
        due = t0 + n * 0.01
        d = due - time.monotonic()
        if d > 0:
            time.sleep(d)
        samples = []
        for i in range(480):
            v = int(amp * math.sin(2 * math.pi * a.tone * (n * 480 + i) / 48000)) if amp else 0
            samples += [v, v]
        s.sendall(frame(0x20, struct.pack("<960h", *samples)))
        n += 1
        if n % 100 == 0:
            s.sendall(frame(0x04, struct.pack("<Q", now_us())))
        while select.select([s], [], [], 0)[0]:
            data = s.recv(1 << 16)
            if not data:
                sys.exit("closed by the receiver")
            buf += data
            while len(buf) >= 16:
                ln, t, fl, tile, pts = HDR.unpack_from(buf)
                if len(buf) < 16 + ln:
                    break
                p = buf[16:16 + ln]
                buf = buf[16 + ln:]
                if t == 0x04:
                    s.sendall(frame(0x05, p))
                elif t == 0x21:
                    if ln != 960:
                        mic_bad += 1
                    pcm = struct.unpack(f"<{ln // 2}h", p)
                    mic_n += 1
                    mic_sq += sum(x * x for x in pcm) / len(pcm)
                    mic_peak = max(mic_peak, max(abs(x) for x in pcm))
                elif t in (0x01, 0x03, 0x06):
                    print({1: "HELLO", 3: "CONFIG_ACK", 6: "BYE"}[t], p.decode()[:160], flush=True)
        if time.monotonic() - last_print >= 1:
            last_print = time.monotonic()
            rms = math.sqrt(mic_sq / mic_n) if mic_n else 0
            db = 20 * math.log10(rms / 32768) if rms else -120
            print(f"sent {n} AUDIO_OUT | AUDIO_IN last second: {mic_n} packets ({mic_bad} wrong size), "
                  f"mic {db:.1f} dBFS rms, peak {mic_peak}", flush=True)
            mic_n, mic_sq, mic_peak, mic_bad = 0, 0.0, 0, 0
    s.sendall(jframe(0x06, {"reason": "send_audio.py done"}))


if __name__ == "__main__":
    main()
