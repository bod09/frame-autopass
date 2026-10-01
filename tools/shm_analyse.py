#!/usr/bin/env python3
"""Offline analysis of a shm_capture.py recording (runs on the PC).

Ground truth is the colour light value (g4) read from the
VR_CameraPassthroughState file, available only while RGB is selected.
Every 32-bit word of every other captured buffer is correlated with it
during the RGB phase; the best candidates are then printed across the
whole run so we can see whether they still follow the lights while mono
is selected.

Usage: shm_analyse.py CAPTURE [--top N] [--min-r R]
"""

import argparse
import struct
import zlib
from collections import defaultdict

import numpy as np

STATE_SIZE = 0x6200
GAMMA = bytes.fromhex("2fbae83e")


def load(path):
    series = defaultdict(list)
    with open(path, "rb") as f:
        data = f.read()
    pos = 0
    while pos < len(data):
        (n,) = struct.unpack_from("<I", data, pos)
        pos += 4
        name = data[pos:pos + n].decode()
        pos += n
        t, raw_len, z_len = struct.unpack_from("<dII", data, pos)
        pos += 16
        buf = zlib.decompress(data[pos:pos + z_len])
        pos += z_len
        if len(buf) == raw_len:
            series[name].append((t, buf))
    return series


def colour_light(buf):
    best = None
    for pos in range(0x20E8 + 0xD8, STATE_SIZE - 0x18, 4):
        if buf[pos:pos + 4] == GAMMA:
            ts = struct.unpack_from("<d", buf, pos - 0xD8 + 0x10)[0]
            light = struct.unpack_from("<f", buf, pos + 0x14)[0]
            if best is None or ts > best[0]:
                best = (ts, light)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--min-r", type=float, default=0.8)
    args = ap.parse_args()

    series = load(args.capture)
    state_name = next(n for n, s in series.items() if len(s[0][1]) == STATE_SIZE)
    state = series.pop(state_name)

    # Ground truth: g4 while RGB is selected and the colour record is fresh.
    truth_t, truth_v, mono_t = [], [], []
    last_ts = None
    for t, buf in state:
        rgb = buf[4]
        if not rgb:
            mono_t.append(t)
            continue
        cl = colour_light(buf)
        if cl and cl[0] != last_ts:
            truth_t.append(t)
            truth_v.append(cl[1])
            last_ts = cl[0]
    truth_t = np.array(truth_t)
    truth_v = np.array(truth_v)
    print(f"state file {state_name}: {len(state)} snapshots, {len(truth_t)} fresh RGB light samples, "
          f"mono from {min(mono_t, default=float('nan')):.1f} to {max(mono_t, default=float('nan')):.1f} s")
    print("light over time (RGB phase):", " ".join(f"{t:.0f}s={v:.2f}" for t, v in zip(truth_t[::8], truth_v[::8])))

    candidates = []
    for name, snaps in series.items():
        times = np.array([t for t, _ in snaps])
        rgb_idx = [i for i, t in enumerate(times) if truth_t.size and truth_t[0] <= t <= truth_t[-1]
                   and not any(abs(t - m) < 0.5 for m in mono_t[:1] + mono_t[-1:])
                   and not (mono_t and mono_t[0] - 1 <= t <= mono_t[-1] + 1)]
        if len(rgb_idx) < 6:
            continue
        size = len(snaps[0][1]) // 4 * 4
        mat = np.frombuffer(b"".join(snaps[i][1][:size] for i in rgb_idx), dtype="<f4").reshape(len(rgb_idx), -1)
        ref = np.interp(times[rgb_idx], truth_t, truth_v)
        with np.errstate(all="ignore"):
            finite = np.isfinite(mat).all(axis=0)
            m = np.where(finite, mat, 0).astype(np.float64)
            m -= m.mean(axis=0)
            r0 = ref - ref.mean()
            denom = np.sqrt((m ** 2).sum(axis=0) * (r0 ** 2).sum())
            r = np.where((denom > 0) & finite, (m * r0[:, None]).sum(axis=0) / denom, 0)
        for w in np.argsort(-np.abs(r))[:args.top]:
            if abs(r[w]) >= args.min_r:
                candidates.append((abs(r[w]), r[w], name, int(w) * 4))

    candidates.sort(reverse=True)
    print(f"\n{len(candidates)} words with |r| >= {args.min_r} against the RGB light value")
    for absr, r, name, off in candidates[:args.top]:
        snaps = series[name]
        vals = [struct.unpack_from("<f", b, off)[0] for _, b in snaps]
        rgb_vals = [v for (t, _), v in zip(snaps, vals) if not (mono_t and mono_t[0] <= t <= mono_t[-1])]
        mono_vals = [v for (t, _), v in zip(snaps, vals) if mono_t and mono_t[0] <= t <= mono_t[-1]]
        print(f"r={r:+.3f} {name.split('/')[-1]}+{off:#x} rgb range {min(rgb_vals):.4g}..{max(rgb_vals):.4g}"
              f" | mono range {min(mono_vals, default=float('nan')):.4g}..{max(mono_vals, default=float('nan')):.4g}")
    return candidates


if __name__ == "__main__":
    main()
