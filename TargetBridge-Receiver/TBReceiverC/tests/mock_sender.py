#!/usr/bin/env python3
"""Mock TargetBridge sender — drives a receiver over plain TCP with no
Thunderbolt hardware, no Apple Silicon, and no screen capture.

Speaks the wire protocol from src/proto.h:
    [4 bytes BE uint32 length][1 byte type][payload (length-1 bytes)]

Modes (--mode):
    handshake   connect, HELLO, heartbeat for --duration, TEARDOWN, close.
    stream      handshake + generate H.264 with the ffmpeg CLI (testsrc2)
                and stream it as PARAM_SETS + AVCC FRAMEs at ~30 fps.
    hang        connect, HELLO, then go silent (no heartbeats) while keeping
                the socket open — exercises the receiver's idle watchdog,
                which should reap the session after ~10s.
    badlen      connect, HELLO, then send a corrupt 0xFFFFFFFF length prefix —
                the receiver's parser must reject it and disconnect.
    drop        connect, HELLO, send half a packet, then close abruptly —
                the receiver must return to "waiting for sender".
    pace        a sender that outruns the decoder: all-intra H.264 at --fps
                (default 60) plus a 120 Hz overlay cursor, for --duration.
                By default it paces itself by the receiver's frame acks
                (proto.h 0x39) the way the real sender does: at most
                --window frames unacknowledged, the newest waiting frame
                held, older ones dropped. With --no-acks it behaves like an
                older sender and just writes every frame, leaving the pacing
                to TCP. Run the receiver with TB_RECEIVER_DECODE_DELAY_MS=30
                to make it the slow side. Reports how long the receiver
                needed to catch up after the last frame; cursor packets
                carry their scheduled send time as "echo", so the receiver's
                [latency] rtt line is how late each one was handled.

Stdlib only. Example:
    python3 tests/mock_sender.py --mode handshake --duration 3
"""

import argparse
import json
import socket
import struct
import subprocess
import sys
import tempfile
import time

# Packet types (src/proto.h)
PKT_HELLO_RECEIVER = 0x10
PKT_PARAM_SETS = 0x20
PKT_FRAME = 0x21
PKT_HEARTBEAT = 0x30
PKT_TEARDOWN = 0x31
PKT_CURSOR = 0x32
PKT_FRAME_ACK = 0x39


def packet(ptype: int, payload: bytes) -> bytes:
    return struct.pack(">I", 1 + len(payload)) + bytes([ptype]) + payload


def json_packet(ptype: int, obj) -> bytes:
    return packet(ptype, json.dumps(obj).encode("utf-8"))


def hello_packet(name: str, frame_acks: bool = False) -> bytes:
    hello = {
        "senderName": name,
        "uiLanguage": "en",
        "capturePreset": "standard1440p",
        "captureSource": "desktopMirror",
        "captureWidth": 1280,
        "captureHeight": 720,
        "codec": "h264",
    }
    if frame_acks:
        hello["frameAcks"] = True
    return json_packet(PKT_HELLO_RECEIVER, hello)


def drain_receiver(sock: socket.socket, quiet: bool = False) -> None:
    """Read whatever the receiver sent (DISPLAY_PROFILE etc.) so its writes
    don't back up; log packet types for debugging."""
    sock.setblocking(False)
    try:
        data = sock.recv(65536)
        while data:
            if len(data) >= 5:
                (length,) = struct.unpack(">I", data[:4])
                if not quiet:
                    print(f"[mock] receiver sent type=0x{data[4]:02x} len={length}")
                data = data[4 + length:]
            else:
                break
    except (BlockingIOError, socket.error):
        pass
    finally:
        sock.setblocking(True)


# ---- H.264 generation & AVCC conversion (stream mode) ----------------------

def generate_h264(duration_s: int, fps: int = 30, intra_only: bool = False) -> bytes:
    """Render a test pattern to Annex-B H.264 using the ffmpeg CLI.
    intra_only: every frame is a keyframe, so any frame may be dropped."""
    with tempfile.NamedTemporaryFile(suffix=".h264", delete=False) as tmp:
        path = tmp.name
    cmd = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", f"testsrc2=size=1280x720:rate={fps}:duration={duration_s}",
        "-c:v", "libx264", "-profile:v", "baseline", "-pix_fmt", "yuv420p",
        "-g", "1" if intra_only else str(fps), "-f", "h264", path,
    ]
    subprocess.run(cmd, check=True)
    with open(path, "rb") as f:
        return f.read()


def split_annexb(data: bytes):
    """Split an Annex-B stream into raw NAL units (start codes removed)."""
    nals, i, n = [], 0, len(data)
    while i < n:
        if data[i:i + 4] == b"\x00\x00\x00\x01":
            start = i + 4
        elif data[i:i + 3] == b"\x00\x00\x01":
            start = i + 3
        else:
            i += 1
            continue
        j = start
        while j < n:
            if data[j:j + 4] == b"\x00\x00\x00\x01" or data[j:j + 3] == b"\x00\x00\x01":
                break
            j += 1
        nals.append(data[start:j])
        i = j
    return nals


def stream_video(sock: socket.socket, duration_s: int) -> None:
    fps = 30
    nals = split_annexb(generate_h264(duration_s, fps))

    sps = next(n for n in nals if n and (n[0] & 0x1F) == 7)
    pps = next(n for n in nals if n and (n[0] & 0x1F) == 8)

    # PARAM_SETS payload: [1B codec (1=H264)][1B count] then [4B BE size][bytes] per set.
    params = bytes([1, 2])
    for ps in (sps, pps):
        params += struct.pack(">I", len(ps)) + ps
    sock.sendall(packet(PKT_PARAM_SETS, params))
    print(f"[mock] sent param sets (SPS {len(sps)}B, PPS {len(pps)}B)")

    # Group slices into frames: a frame is one slice NAL (types 1/5) here
    # (baseline testsrc2 output, one slice per picture).
    frame_interval = 1.0 / fps
    heartbeat_due = time.monotonic()
    seq = 0
    frames_sent = 0
    for nal in nals:
        ntype = nal[0] & 0x1F if nal else 0
        if ntype not in (1, 5):
            continue
        # FRAME payload: AVCC — 4-byte BE length-prefixed NAL units.
        sock.sendall(packet(PKT_FRAME, struct.pack(">I", len(nal)) + nal))
        frames_sent += 1
        now = time.monotonic()
        if now >= heartbeat_due:
            sock.sendall(json_packet(PKT_HEARTBEAT, {"sequence": seq}))
            seq += 1
            heartbeat_due = now + 2.0
        drain_receiver(sock, quiet=True)
        time.sleep(frame_interval)
    print(f"[mock] streamed {frames_sent} frames")


# ---- a sender faster than the decoder (pace mode) ---------------------------

def clock_us() -> int:
    """The receiver's latency clock (CLOCK_MONOTONIC, µs) on the same Mac."""
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC) // 1000


class ReceiverPackets:
    """Non-blocking reader for what the receiver sends; tracks frame acks."""

    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.buf = b""
        self.acked = 0
        self.closed = False

    def poll(self) -> None:
        self.sock.setblocking(False)
        try:
            while True:
                data = self.sock.recv(65536)
                if not data:
                    self.closed = True
                    break
                self.buf += data
        except (BlockingIOError, InterruptedError):
            pass
        except OSError:
            self.closed = True
        finally:
            self.sock.setblocking(True)
        while len(self.buf) >= 5:
            (length,) = struct.unpack(">I", self.buf[:4])
            if len(self.buf) < 4 + length:
                break
            ptype, payload = self.buf[4], self.buf[5:4 + length]
            self.buf = self.buf[4 + length:]
            if ptype == PKT_FRAME_ACK:
                self.acked = max(self.acked, int(json.loads(payload)["frames"]))


def pace_prepare(duration_s: int, fps: int):
    """Encodes the test video. Done before connecting: it can take longer
    than the receiver's 10 s idle watchdog allows a silent sender."""
    nals = split_annexb(generate_h264(duration_s, fps, intra_only=True))
    sps = next(n for n in nals if n and (n[0] & 0x1F) == 7)
    pps = next(n for n in nals if n and (n[0] & 0x1F) == 8)
    frames = [n for n in nals if n and (n[0] & 0x1F) in (1, 5)]
    params = bytes([1, 2])
    for ps in (sps, pps):
        params += struct.pack(">I", len(ps)) + ps
    return params, frames


def pace_stream(sock: socket.socket, prepared, fps: int, use_acks: bool, window: int) -> int:
    params, frames = prepared
    sock.sendall(packet(PKT_PARAM_SETS, params))
    avg_kb = sum(len(f) for f in frames) / max(1, len(frames)) / 1024
    print(f"[mock] {len(frames)} frames at {fps} fps, {avg_kb:.1f} KB each, "
          f"pacing={'acks window ' + str(window) if use_acks else 'none (TCP only)'}")

    receiver = ReceiverPackets(sock)
    frame_interval_us = 1_000_000 // fps
    cursor_interval_us = 1_000_000 // 120
    start = clock_us()
    next_frame, next_cursor = start, start
    index = sent = dropped = cursors = 0
    held = None
    worst_unacked = 0

    def send_frame(nal: bytes) -> None:
        nonlocal sent, worst_unacked
        sock.sendall(packet(PKT_FRAME, struct.pack(">I", len(nal)) + nal))
        sent += 1
        worst_unacked = max(worst_unacked, sent - receiver.acked)

    while (index < len(frames) or held is not None) and not receiver.closed:
        now = clock_us()
        receiver.poll()
        if now >= next_cursor:
            # A cursor sweeping across the picture; "echo" is when it was
            # due, so time lost queueing behind video in this process counts.
            x = 100 + (cursors * 7) % 1000
            sock.sendall(json_packet(PKT_CURSOR, {
                "x": x, "y": 360, "width": 1280, "height": 720, "visible": True,
                "type": 0, "large": False, "echo": next_cursor,
            }))
            cursors += 1
            next_cursor += cursor_interval_us
        if index < len(frames) and now >= next_frame:
            nal = frames[index]
            index += 1
            next_frame += frame_interval_us
            if not use_acks:
                send_frame(nal)
            elif sent - receiver.acked < window:
                if held is not None:
                    dropped += 1
                    held = None
                send_frame(nal)
            else:
                if held is not None:
                    dropped += 1
                held = nal
        if use_acks and held is not None and sent - receiver.acked < window:
            send_frame(held)
            held = None
        time.sleep(0.0005)

    # The receiver handles TEARDOWN only after everything sent before it, so
    # the wait for it to close is how far behind the picture still was.
    stopped = clock_us()
    try:
        sock.sendall(json_packet(PKT_TEARDOWN, {"reason": "mock pace done"}))
        sock.settimeout(60)
        while sock.recv(65536):
            pass
    except OSError:
        pass
    catch_up_ms = (clock_us() - stopped) / 1000
    print(f"[mock] sent {sent} frames ({dropped} dropped here before sending), {cursors} cursor packets; "
          f"most unacked {worst_unacked if use_acks else 'n/a'}")
    print(f"[mock] receiver caught up {catch_up_ms:.0f} ms after the last frame was sent")
    return 0


# ---- modes ------------------------------------------------------------------

def run(args) -> int:
    prepared = pace_prepare(args.duration, args.fps) if args.mode == "pace" else None
    sock = socket.create_connection((args.host, args.port), timeout=5)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"[mock] connected to {args.host}:{args.port}")
    time.sleep(0.2)
    drain_receiver(sock)

    sock.sendall(hello_packet(args.name, frame_acks=args.mode == "pace" and not args.no_acks))
    print("[mock] sent HELLO")

    try:
        if args.mode == "handshake":
            deadline = time.monotonic() + args.duration
            seq = 0
            while time.monotonic() < deadline:
                sock.sendall(json_packet(PKT_HEARTBEAT, {"sequence": seq}))
                seq += 1
                drain_receiver(sock)
                time.sleep(2.0)
            sock.sendall(json_packet(PKT_TEARDOWN, {"reason": "mock done"}))
            print("[mock] sent TEARDOWN")

        elif args.mode == "stream":
            stream_video(sock, args.duration)
            sock.sendall(json_packet(PKT_TEARDOWN, {"reason": "mock stream done"}))

        elif args.mode == "pace":
            return pace_stream(sock, prepared, args.fps, not args.no_acks, args.window)

        elif args.mode == "hang":
            print(f"[mock] going silent for {args.duration}s (socket stays open, no heartbeats)")
            time.sleep(args.duration)
            # If the receiver's idle watchdog works, it already closed us.
            try:
                sock.sendall(json_packet(PKT_HEARTBEAT, {"sequence": 0}))
                sock.sendall(json_packet(PKT_HEARTBEAT, {"sequence": 1}))
                print("[mock] receiver still accepted writes after silence")
            except (BrokenPipeError, ConnectionResetError):
                print("[mock] receiver closed the stale session (watchdog OK)")

        elif args.mode == "badlen":
            sock.sendall(struct.pack(">I", 0xFFFFFFFF) + b"\x21")
            print("[mock] sent corrupt 0xFFFFFFFF length prefix")
            sock.settimeout(5)
            try:
                data = sock.recv(4096)
                while data:
                    data = sock.recv(4096)
                print("[mock] receiver disconnected us (parser rejected corrupt length)")
            except socket.timeout:
                print("[mock] ERROR: receiver did not disconnect within 5s")
                return 1

        elif args.mode == "drop":
            # Announce a 1000-byte packet but send only half, then vanish.
            sock.sendall(struct.pack(">I", 1001) + b"\x21" + b"\x00" * 500)
            print("[mock] sent partial frame; closing abruptly")

    finally:
        sock.close()
    print("[mock] done")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=54321)
    ap.add_argument("--mode", choices=["handshake", "stream", "pace", "hang", "badlen", "drop"], default="handshake")
    ap.add_argument("--duration", type=int, default=5, help="seconds (mode-specific)")
    ap.add_argument("--name", default="MockSender")
    ap.add_argument("--fps", type=int, default=60, help="pace mode: frames per second offered")
    ap.add_argument("--no-acks", action="store_true", help="pace mode: ignore frame acks, like an older sender")
    ap.add_argument("--window", type=int, default=2, help="pace mode: frames allowed unacknowledged")
    return run(ap.parse_args())


if __name__ == "__main__":
    sys.exit(main())
