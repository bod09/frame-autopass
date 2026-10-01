#!/usr/bin/env python3
"""Record every new per-frame record of both passthrough streams.

Passive: only reads SteamVR's VR_CameraPassthroughState tmpfs file (the
25088-byte /dev/shm segment); no SteamVR calls, no devices. For each
stream and camera, whenever the newest record's timestamp changes, one
line is written: wall time, stream (mono/colour), camera, record index,
timestamp and the record's raw bytes as hex. Used to look for per-frame
fields that follow the IR emitters (FINDINGS.md 38).

Usage: shm_records.py --seconds N [--hz N] --out PATH
"""

import argparse
import glob
import os
import struct
import sys
import time

STATE_SIZE = 0x6200
STREAMS = {"mono": 0x14, "colour": 0x20E8}
CAMS, RECS, REC_SIZE, CAM_SIZE = 2, 16, 0x104, 0x1040


def find_state_file():
    m = [p for p in glob.glob("/dev/shm/u1000-Shm_*") if os.path.getsize(p) == STATE_SIZE]
    if len(m) != 1:
        sys.exit(f"expected one {STATE_SIZE}-byte shm file, found {m}")
    return m[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, required=True)
    ap.add_argument("--hz", type=float, default=50)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    path = find_state_file()
    last = {}
    end = time.time() + a.seconds
    with open(path, "rb") as f, open(a.out, "w") as out:
        while time.time() < end:
            f.seek(0)
            buf = f.read(STATE_SIZE)
            now = time.time()
            out.write(f"{now:.3f} cfg {buf[2:7].hex()}\n") if last.get("cfg") != buf[2:7] else None
            last["cfg"] = buf[2:7]
            for name, base in STREAMS.items():
                for cam in range(CAMS):
                    best = None
                    for rec in range(RECS):
                        off = base + 0x38 + cam * CAM_SIZE + rec * REC_SIZE
                        ts = struct.unpack_from("<d", buf, off + 0x10)[0]
                        if ts == ts and (best is None or ts > best[0]):
                            best = (ts, rec, off)
                    if best and last.get((name, cam)) != best[0]:
                        last[(name, cam)] = best[0]
                        ts, rec, off = best
                        out.write(f"{now:.3f} {name} {cam} {rec} {ts:.6f} {buf[off:off + REC_SIZE].hex()}\n")
            time.sleep(1 / a.hz)


if __name__ == "__main__":
    main()
