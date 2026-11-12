#!/usr/bin/env python3
"""Summarise a dd2recomp --scroll-log file.

    python3 tools/scroll_pattern.py scroll.log [--from FRAME] [--to FRAME]

Prints the per-frame dx string, how often the camera updates, the step sizes
it moves by, and the most common 4-update groups (a 2,2,3,1 hitch every
8 frames shows up as "2 2 3 1" with updates every 2 frames).
"""
import argparse
import collections
import sys


def read(path, lo, hi):
    rows = []
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                if "state loaded" in line or "power on" in line:
                    rows.append(None)          # break in continuity
                continue
            p = line.split()
            if len(p) < 2 or p[1] == "off":
                rows.append(None)
                continue
            frame, x, dx = int(p[0]), int(p[1]), int(p[2])
            if dx <= -128:
                dx += 256                    # logs from before the modulo-256 fix
            if lo <= frame <= hi:
                rows.append((frame, x, dx))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--from", dest="lo", type=int, default=0)
    ap.add_argument("--to", dest="hi", type=int, default=10**9)
    a = ap.parse_args()
    rows = read(a.log, a.lo, a.hi)
    if not any(rows):
        sys.exit("no rendered frames in range")

    # Segments of continuous rendering that contain movement.
    segs, cur = [], []
    for r in rows + [None]:
        if r is None:
            if any(dx for _, _, dx in cur):
                segs.append(cur)
            cur = []
        else:
            cur.append(r)

    total_moves = collections.Counter()
    gaps = collections.Counter()
    groups = collections.Counter()
    gap_groups = collections.Counter()
    big = []
    for seg in segs:
        # Trim idle frames at both ends.
        idx = [i for i, (_, _, dx) in enumerate(seg) if dx]
        seg = seg[idx[0]: idx[-1] + 1]
        print(f"frames {seg[0][0]}-{seg[-1][0]}  x {seg[0][1]}->{seg[-1][1]}")
        line = " ".join(str(dx) for _, _, dx in seg)
        print("  dx per frame:", line if len(line) < 400 else line[:400] + " ...")
        moves = [(f, dx) for f, _, dx in seg if dx]
        steps = [dx for _, dx in moves]
        total_moves.update(steps)
        seg_gaps = []
        for (f0, _), (f1, _) in zip(moves, moves[1:]):
            gaps[f1 - f0] += 1
            seg_gaps.append(f1 - f0)
        for i in range(len(seg_gaps) - 3):
            g = tuple(seg_gaps[i:i + 4])
            if max(g) <= 4:                  # skip pauses (wave locks)
                gap_groups[g] += 1
        big += [(f, dx) for f, dx in moves if abs(dx) > 4]
        for i in range(len(steps) - 3):
            groups[tuple(steps[i:i + 4])] += 1
        frames = seg[-1][0] - seg[0][0] + 1
        dist = sum(steps)
        print(f"  {len(steps)} updates over {frames} frames, {dist} px, {dist / frames:.3f} px/frame")

    print("\nstep sizes:   ", dict(sorted(total_moves.items())))
    print("frames between updates:", dict(sorted(gaps.items())))
    print("most common 4-step groups (px):")
    for g, n in groups.most_common(6):
        print(f"  {' '.join(map(str, g)):>12}  x{n}")
    print("most common 4-gap groups (frames between updates):")
    for g, n in gap_groups.most_common(6):
        print(f"  {' '.join(map(str, g)):>12}  x{n}")
    if big:
        print("steps over 4 px:", ", ".join(f"frame {f}: {dx:+d}" for f, dx in big))


if __name__ == "__main__":
    main()
