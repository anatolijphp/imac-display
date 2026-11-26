#!/usr/bin/env python3
"""A stand-in for imacdisplayd, to test the Mac side without the iMac (the counterpart of
receiver/test/fake_mac.py). Speaks PROTOCOL.md version 0 on one port: answers HELLO, announces
one fake drive before CONFIG_ACK (as the real receiver does), answers PING, asks for a keyframe
once, and writes every tile's video to <out>/tileN.h264 so it can be checked with ffprobe.

    test/fake_imac.py --port 7501 --out /tmp/fake &
    .build/release/imd pipeline --host localhost --port 7501 --capture 3840x2160 --seconds 5
"""
import argparse, json, os, socket, struct, threading, time

HELLO, CONFIG, CONFIG_ACK, PING, PONG, BYE = 1, 2, 3, 4, 5, 6
VIDEO_FRAME, VIDEO_REQUEST_KEYFRAME = 0x10, 0x11
INPUT_KEY, STORAGE_ATTACHED = 0x30, 0x40
lock = threading.Lock()


def now_us():
    return time.monotonic_ns() // 1000


def send(sock, mtype, payload=b"", flags=0, tile=0):
    if isinstance(payload, dict):
        payload = json.dumps(payload).encode()
    with lock:
        sock.sendall(struct.pack("<IBBHQ", len(payload), mtype, flags, tile, now_us()) + payload)


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return bytes(buf)


def recv(sock):
    length, mtype, flags, tile, pts = struct.unpack("<IBBHQ", recv_exact(sock, 16))
    return mtype, flags, tile, pts, recv_exact(sock, length) if length else b""


def ctrl(sock, args):
    send(sock, HELLO, {"proto": 0, "role": "imac", "panel": {"w": 5120, "h": 2880, "hz": 60},
                       "decode": {"codecs": ["h264"], "max_w": 3840, "max_h": 2160, "max_tiles": 4, "hw": False},
                       "audio": {"out": False, "in": False}, "features": ["input", "storage"]})
    send(sock, STORAGE_ATTACHED, {"id": "fake-1", "label": "FAKE", "fs": "exfat", "size": 1 << 30, "read_only": True,
                                  "share": "usb-fake-1", "user": "imac", "password": "not-a-real-share"})
    started = time.time()
    asked = False
    sock.settimeout(1.0)
    while True:
        try:
            mtype, _, _, _, payload = recv(sock)
        except socket.timeout:
            mtype = None
        if mtype == CONFIG:
            print("ctrl: CONFIG", payload.decode())
            send(sock, CONFIG_ACK, {"ok": True})
            send(sock, INPUT_KEY, struct.pack("<HHB3x", 0x07, 0x04, 1))   # the A key, down
        elif mtype == PING:
            send(sock, PONG, payload)
        elif mtype == BYE:
            print("ctrl: BYE", payload.decode())
            return
        elif mtype is not None and mtype != PONG:
            print(f"ctrl: type 0x{mtype:02x}, {len(payload)} bytes")
        if not asked and args.keyframe_after and time.time() - started > args.keyframe_after:
            send(sock, VIDEO_REQUEST_KEYFRAME)
            asked = True


def video(sock, args):
    send(sock, HELLO, {"proto": 0, "role": "imac"})
    files, frames, keys, pictures, order_errors, last_tile = {}, 0, 0, 0, 0, -1
    try:
        while True:
            mtype, flags, tile, pts, payload = recv(sock)
            if mtype != VIDEO_FRAME:
                continue
            frames += 1
            keys += flags & 1
            if tile != last_tile + 1:
                order_errors += 1
            last_tile = -1 if flags & 2 else tile
            pictures += 1 if flags & 2 else 0
            if not payload.startswith(b"\x00\x00\x00\x01"):
                print("video: frame without a start code")
            if args.out:
                if tile not in files:
                    files[tile] = open(os.path.join(args.out, f"tile{tile}.h264"), "wb")
                files[tile].write(payload)
    except ConnectionError:
        pass
    for f in files.values():
        f.close()
    print(f"video: {frames} frames, {pictures} pictures, {keys} keyframes, {len(files)} tile file(s), {order_errors} out of order")


def handle(sock, args):
    try:
        mtype, _, _, _, payload = recv(sock)
        hello = json.loads(payload) if mtype == HELLO else {}
        print("HELLO", hello)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        (video if hello.get("channel") == "video" else ctrl)(sock, args)
    except ConnectionError:
        print("connection closed")
    finally:
        sock.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=7501)
    ap.add_argument("--out", help="folder for tileN.h264")
    ap.add_argument("--keyframe-after", type=float, default=2.0, help="send VIDEO_REQUEST_KEYFRAME after this many seconds (0 = never)")
    args = ap.parse_args()
    if args.out:
        os.makedirs(args.out, exist_ok=True)
    srv = socket.socket(socket.AF_INET6)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
    srv.bind(("::", args.port))
    srv.listen(4)
    print(f"fake iMac listening on port {args.port}")
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle, args=(conn, args), daemon=True).start()


if __name__ == "__main__":
    main()
