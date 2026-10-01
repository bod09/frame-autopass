#!/usr/bin/env python3
"""Passively capture every shared-memory file a process maps.

Reads tmpfs files only: no SteamVR calls, no locks, no devices. Used to
find which of XRService's shared buffers carry colour-camera statistics
while mono passthrough is displayed.

Each snapshot is appended to the output as a record:
    u32 name_len, name, f64 t, u32 raw_len, u32 z_len, zlib(data)
Small files are read every --fast seconds, large ones every --slow.

Usage: shm_capture.py [--pid PID | --process NAME] [--seconds N]
                      [--fast S] [--slow S] [--large BYTES] [--also PATH] --out PATH
"""

import argparse
import os
import re
import struct
import subprocess
import time
import zlib


def find_pid(pattern):
    out = subprocess.run(["pgrep", "-f", pattern], capture_output=True, text=True).stdout.split()
    pids = [int(p) for p in out if int(p) != os.getpid()]
    if not pids:
        raise SystemExit(f"no process matches {pattern!r}")
    return min(pids)


def mapped_shm(pid):
    paths = set()
    with open(f"/proc/{pid}/maps") as f:
        for line in f:
            m = re.search(r"(/dev/shm/\S+)", line)
            if m:
                paths.add(m.group(1))
    for fd in os.listdir(f"/proc/{pid}/fd"):
        try:
            target = os.readlink(f"/proc/{pid}/fd/{fd}")
        except OSError:
            continue
        if target.startswith("/dev/shm/"):
            paths.add(target)
    return sorted(p for p in paths if os.path.isfile(p))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int)
    ap.add_argument("--process", default="XRService --documentsRoot")
    ap.add_argument("--seconds", type=float, default=90)
    ap.add_argument("--fast", type=float, default=0.25)
    ap.add_argument("--slow", type=float, default=2.0)
    ap.add_argument("--large", type=int, default=1 << 20)
    ap.add_argument("--also", action="append", default=[],
                    help="extra file to capture on the fast schedule (repeatable)")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    pid = args.pid or find_pid(args.process)
    paths = sorted(set(mapped_shm(pid)) | set(args.also))
    sizes = {p: os.path.getsize(p) for p in paths}
    print(f"pid {pid}: {len(paths)} shm files, {sum(sizes.values()) / 1e6:.1f} MB total")

    t0 = time.monotonic()
    next_slow = 0.0
    with open(args.out, "wb") as out:
        while True:
            now = time.monotonic() - t0
            if now >= args.seconds:
                break
            do_slow = now >= next_slow
            if do_slow:
                next_slow = now + args.slow
            for p in paths:
                if sizes[p] > args.large and not do_slow:
                    continue
                try:
                    with open(p, "rb") as f:
                        data = f.read()
                except OSError:
                    continue
                z = zlib.compress(data, 1)
                name = p.encode()
                out.write(struct.pack("<I", len(name)) + name + struct.pack("<dII", now, len(data), len(z)) + z)
            spent = time.monotonic() - t0 - now
            time.sleep(max(0.0, args.fast - spent))
    print(f"wrote {os.path.getsize(args.out) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
