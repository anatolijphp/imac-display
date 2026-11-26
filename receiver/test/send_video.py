#!/usr/bin/env python3
"""Test sender: plays the MacBook's part for video. Encodes a 5 s moving test clip with FFmpeg once
(libx264, no B-frames, one reference frame, zero latency), optionally split into tiles, then loops it
to imacdisplayd at exactly --fps with the real protocol (ctrl + video connections).

    send_video.py HOST [--port 7500] [--size 3840x2160] [--tiles 1|2|4] [--fps 60]
                       [--mbps 40] [--seconds 20] [--preset veryfast] [--clip-dir DIR]

One FFmpeg process encodes all tiles from the same source (split filter), so tiles stay in step.
Clips are cached in --clip-dir; encoding ahead keeps the receiver's CPU to itself.
Prints the receiver's VIDEO_STATS once per second.
"""
import argparse, json, os, select, socket, struct, subprocess, sys, time, uuid

HDR = struct.Struct("<IBBHQ")


def now_us():
    return time.monotonic_ns() // 1000


def frame(t, payload=b"", flags=0, tile=0, pts=None):
    return HDR.pack(len(payload), t, flags, tile, now_us() if pts is None else pts) + payload


def jframe(t, obj):
    return frame(t, json.dumps(obj).encode())


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


class FrameReader:
    def __init__(self, sock):
        self.s, self.buf = sock, b""

    def frames(self):
        data = self.s.recv(1 << 16)
        if not data:
            raise EOFError
        self.buf += data
        while len(self.buf) >= 16:
            n, t, fl, tile, pts = HDR.unpack_from(self.buf)
            if len(self.buf) < 16 + n:
                break
            yield t, self.buf[16:16 + n]
            self.buf = self.buf[16 + n:]


class AnnexB:
    """Splits an Annex B H.264 byte stream into access units at access unit delimiters (NAL type 9)."""

    def __init__(self, fd):
        self.fd, self.buf, self.eof = fd, bytearray(), False

    def _aud(self, start):
        """Position of the next access unit delimiter's start code at or after start, or -1."""
        i = self.buf.find(b"\x00\x00\x01\x09", start)
        if i > 0 and self.buf[i - 1] == 0:
            i -= 1
        return i

    def next_au(self):
        while True:
            first = self._aud(0)
            second = self._aud(first + 5) if first >= 0 else -1
            if second >= 0:
                au = bytes(self.buf[first:second])
                del self.buf[:second]
                return au
            if self.eof:
                if first >= 0 and len(self.buf) - first > 8:
                    au = bytes(self.buf[first:])
                    self.buf.clear()
                    return au
                return None
            chunk = os.read(self.fd, 1 << 20)
            if not chunk:
                self.eof = True
            self.buf += chunk


def is_idr(au):
    i = 0
    while True:
        i = au.find(b"\x00\x00\x01", i)
        if i < 0 or i + 3 >= len(au):
            return False
        if au[i + 3] & 0x1F == 5:
            return True
        i += 3


def tile_layout(w, h, n):
    cols, rows = {1: (1, 1), 2: (2, 1), 4: (2, 2)}[n]
    tw, th = w // cols, h // rows
    return [{"id": r * cols + c, "x": c * tw, "y": r * th, "w": tw, "h": th} for r in range(rows) for c in range(cols)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=7500)
    ap.add_argument("--size", default="3840x2160")
    ap.add_argument("--tiles", type=int, default=1, choices=(1, 2, 4))
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--mbps", type=float, default=40)
    ap.add_argument("--seconds", type=float, default=20)
    ap.add_argument("--preset", default="veryfast")
    ap.add_argument("--clip-dir", default="/root/build/clips")
    ap.add_argument("--reconfig-after", type=float, default=0,
                    help="send the CONFIG again after this many seconds (like a resolution change), then a keyframe")
    ap.add_argument("--partial", action="store_true",
                    help="leave tiles out: tile 0 in every picture, the others in every second picture")
    ap.add_argument("--clip", help="stream this Annex B H.264 file as one tile (needs access unit delimiters: "
                                   "ffmpeg -i IN -c copy -bsf:v h264_metadata=aud=insert -f h264 OUT); --size must match")
    a = ap.parse_args()
    W, H = map(int, a.size.split("x"))
    tiles = tile_layout(W, H, a.tiles)

    # Encode once, ahead of time, so the receiver's CPU is not shared with a live encoder: a 5 s clip per
    # tile (one source, split and cropped, so tiles stay in step), cached under --clip-dir and looped.
    n = len(tiles)
    clip_dir = os.path.join(a.clip_dir, f"{W}x{H}-{n}t-{a.mbps:g}M-{a.fps}fps-{a.preset}")
    paths = [os.path.join(clip_dir, f"tile{i}.h264") for i in range(n)]
    if a.clip:
        if n != 1:
            sys.exit("--clip needs --tiles 1")
        paths = [a.clip]
    elif not all(os.path.exists(p) for p in paths):
        os.makedirs(clip_dir, exist_ok=True)
        print(f"encoding test clip into {clip_dir} (once)...", flush=True)
        src = (f"drawtext=fontfile=/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf:fontsize={H // 12}:"
               f"fontcolor=white:box=1:boxcolor=black@0.6:x=(w-tw)/2:y=h*0.42:text='%{{n}}'")
        graph = [f"[0]{src},split={n}" + "".join(f"[s{i}]" for i in range(n))]
        for i, t in enumerate(tiles):
            graph.append(f"[s{i}]crop={t['w']}:{t['h']}:{t['x']}:{t['y']}[o{i}]")
        cmd = ["ffmpeg", "-loglevel", "error", "-y", "-t", "5", "-f", "lavfi",  # -t before -i: limits the source
               "-i", f"testsrc2=size={W}x{H}:rate={a.fps}", "-filter_complex", ";".join(graph)]
        per_tile = a.mbps * 1e6 / n
        for i, p in enumerate(paths):
            cmd += ["-map", f"[o{i}]", "-c:v", "libx264", "-preset", a.preset, "-tune", "zerolatency",
                    "-bf", "0", "-refs", "1", "-g", str(a.fps), "-pix_fmt", "yuv420p",
                    "-b:v", str(int(per_tile)), "-maxrate", str(int(per_tile)),
                    "-bufsize", str(int(per_tile / a.fps * 2)),
                    "-bsf:v", "h264_metadata=aud=insert", "-f", "h264", p]
        subprocess.run(cmd, check=True)
    clips = []
    for p in paths:
        r = AnnexB(os.open(p, os.O_RDONLY))
        aus = []
        while (au := r.next_au()) is not None:
            aus.append(au)
        clips.append(aus)
    nframes = min(len(c) for c in clips)

    session = str(uuid.uuid4())
    ctrl = connect(a.host, a.port)
    ctrl.sendall(jframe(0x01, {"proto": 0, "role": "mac", "channel": "ctrl", "session": session, "name": "send_video.py"}))
    cfg = {"video": {"codec": "h264", "fps": a.fps, "w": W, "h": H, "tiles": tiles, "scale": "fit"}}
    ctrl.sendall(jframe(0x02, cfg))
    video = connect(a.host, a.port)
    video.sendall(jframe(0x01, {"proto": 0, "role": "mac", "channel": "video", "session": session}))
    rd = FrameReader(ctrl)
    vrd = FrameReader(video)
    print(f"sending {W}x{H} at {a.fps} fps in {n} tile(s), {a.mbps} Mbit/s total, x264 {a.preset}, "
          f"{nframes}-frame clip looped", flush=True)

    end = time.monotonic() + a.seconds
    t0 = time.monotonic()
    pos = [0] * n  # next frame of each tile's clip
    reconfigured = False
    next_ping = 0
    sent = 0
    try:
        while time.monotonic() < end:
            # Pace at exactly fps, like a display refresh on the Mac.
            due = t0 + sent / a.fps
            delay = due - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            if a.reconfig_after and not reconfigured and time.monotonic() - t0 > a.reconfig_after:
                reconfigured = True
                print("sending CONFIG again (resolution change)", flush=True)
                ctrl.sendall(jframe(0x02, cfg))
                time.sleep(0.15)
                pos = [0] * n  # first picture after a CONFIG is a keyframe
            # Which tiles go into this picture. Each tile is its own stream, so a tile that is left out
            # simply continues with its next frame the next time it is sent.
            send = [i for i in range(n) if not a.partial or i == 0 or sent % 2 == 0]
            pts = now_us()
            for j, i in enumerate(send):
                au = clips[i][pos[i] % nframes]  # loop the clip; frame 0 is a keyframe
                pos[i] += 1
                flags = (1 if is_idr(au) else 0) | (2 if j == len(send) - 1 else 0)
                video.sendall(frame(0x10, au, flags=flags, tile=tiles[i]["id"], pts=pts))
            sent += 1
            now = time.monotonic()
            if now >= next_ping:
                ctrl.sendall(frame(0x04, struct.pack("<Q", now_us())))
                next_ping = now + 1
            for s in select.select([ctrl, video], [], [], 0)[0]:
                for t, p in (rd if s is ctrl else vrd).frames():
                    if t == 0x04:
                        ctrl.sendall(frame(0x05, p))
                    elif t == 0x12:
                        print(f"[{sent:5d} sent] VIDEO_STATS {p.decode()}", flush=True)
                    elif t == 0x11:
                        # Like the Mac: next picture has a keyframe for every tile (restart the clips).
                        print("VIDEO_REQUEST_KEYFRAME: restarting the clip at its keyframe", flush=True)
                        pos = [0] * n
                    elif t in (0x01, 0x03, 0x06):
                        print({1: "HELLO", 3: "CONFIG_ACK", 6: "BYE"}[t], p.decode()[:200], flush=True)
        ctrl.sendall(jframe(0x06, {"reason": "send_video.py done"}))
    except (EOFError, ConnectionError, BrokenPipeError) as e:
        print("connection closed:", e)
    finally:
        pass


if __name__ == "__main__":
    main()
