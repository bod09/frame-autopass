#!/usr/bin/env python3
"""Passively sample SteamVR's VR_CameraPassthroughState shared memory.

Only reads a tmpfs file. It makes no SteamVR calls, takes no locks and
opens no devices, so it cannot disturb passthrough or tracking. Readings
may occasionally tear (the writer holds a mutex we deliberately ignore);
that is acceptable for exploration.

Per-frame records are found by their gamma marker (1/2.2 as float32).
For each record whose frame counter changed, one CSV row is written with
the raw fields around the marker, so we can see which ones track light.

With --raw PATH, every snapshot is also appended to PATH as
[float64 time][STATE_SIZE bytes] so it can be re-parsed offline.

Usage: shm_sampler.py [--seconds N] [--hz N] [--out PATH] [--raw PATH] [--shm PATH]
"""

import argparse
import glob
import os
import struct
import sys
import time

GAMMA = struct.pack("<f", 1 / 2.2)
STATE_SIZE = 0x6200
RECORD_BEFORE_GAMMA = 0xD8   # record start is this far before the marker
FIELDS_AFTER_GAMMA = 12      # float32 fields logged after the marker


def find_state_file():
    matches = [p for p in glob.glob("/dev/shm/u1000-Shm_*") if os.path.getsize(p) == STATE_SIZE]
    if len(matches) != 1:
        sys.exit(f"expected exactly one {STATE_SIZE}-byte shm file, found {matches}")
    return matches[0]


def records(buf):
    pos = buf.find(GAMMA)
    while pos != -1:
        start = pos - RECORD_BEFORE_GAMMA
        if start >= 0:
            yield start, pos
        pos = buf.find(GAMMA, pos + 4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--hz", type=float, default=20)
    ap.add_argument("--out", default="shm_samples.csv")
    ap.add_argument("--raw")
    ap.add_argument("--shm")
    args = ap.parse_args()
    path = args.shm or find_state_file()

    header = ["t", "offset", "stream", "id", "frame", "ts0", "ts1", "fx", "fy", "cx", "cy"]
    header += [f"g{i}" for i in range(FIELDS_AFTER_GAMMA)] + ["owner", "seq", "config"]
    last_frame = {}
    t0 = time.monotonic()
    raw = open(args.raw, "wb") if args.raw else None
    with open(args.out, "w") as out, open(path, "rb") as f:
        out.write(",".join(header) + "\n")
        while time.monotonic() - t0 < args.seconds:
            f.seek(0)
            buf = f.read(STATE_SIZE)
            now = time.monotonic() - t0
            if raw:
                raw.write(struct.pack("<d", now) + buf)
            config = buf[2:7].hex()
            for start, g in records(buf):
                ident, frame = struct.unpack_from("<II", buf, start)
                owner, seq = struct.unpack_from("<II", buf, g + 4 + FIELDS_AFTER_GAMMA * 4)
                key = start
                if last_frame.get(key) == (frame, seq):
                    continue
                last_frame[key] = (frame, seq)
                ts0, ts1 = struct.unpack_from("<dd", buf, start + 0x10)
                fx, fy, cx, cy = struct.unpack_from("<4f", buf, start + 0x20)
                fields = struct.unpack_from(f"<{FIELDS_AFTER_GAMMA}f", buf, g + 4)
                stream = 1 if start >= 0x20E8 else 0
                row = [f"{now:.3f}", hex(start), str(stream), str(ident), str(frame), f"{ts0:.4f}", f"{ts1:.4f}",
                       f"{fx:.1f}", f"{fy:.1f}", f"{cx:.1f}", f"{cy:.1f}"]
                row += [f"{v:.6g}" for v in fields] + [str(owner), str(seq), config]
                out.write(",".join(row) + "\n")
            time.sleep(1 / args.hz)
    if raw:
        raw.close()


if __name__ == "__main__":
    main()
