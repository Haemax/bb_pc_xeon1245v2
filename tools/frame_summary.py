#!/usr/bin/env python3
"""Summarize a BB_FRAME_LOG csv (the launcher's "Save the log and statistics to a file" writes one
to logs/<time>.frames.csv; by hand: BB_FRAME_STATS=1 BB_FRAME_LOG=frames.csv).

    python3 tools/frame_summary.py logs/20261016_160000.frames.csv [--skip 20] [--compare other.csv]

Prints the frame rate, the frame-time percentiles, the "1 % / 0.1 % low" frame rates (the average
of the slowest 1 % / 0.1 % of the frames) and the stutter counts, then says whether the run looks
GPU-bound or CPU-bound:

  present_wait_ms  time the frame preparation waited for the GPU to finish an earlier frame
                   (BB_FRAMES_AHEAD): large = GPU-bound.
  tick_wait_ms     time blocked in the scheduler on GPU work (fences, ticks).
  stage_a_cpu_ms   CPU time of the command-decoding thread.

--skip N drops the first N seconds (loading, shader compiles) so they do not hide the steady state.
"""
import argparse
import csv
import math
import statistics
import sys


def load(path, skip_s):
    rows = []
    try:
        handle = open(path, newline="")
    except OSError as error:
        sys.exit(f"frame_summary: {error}")
    with handle:
        for row in csv.DictReader(handle):
            try:
                if None in row.values() or None in row:  # a line cut off by a crash (or too long)
                    continue
                values = {k: float(v) for k, v in row.items()}
                if values["t_s"] >= skip_s:
                    rows.append(values)
            except (ValueError, KeyError):
                continue
    return rows


def percentile(sorted_values, fraction):
    if not sorted_values:
        return float("nan")
    index = min(len(sorted_values) - 1, max(0, math.ceil(fraction * len(sorted_values)) - 1))
    return sorted_values[index]


def low_fps(frame_ms_desc, fraction):
    count = max(1, int(len(frame_ms_desc) * fraction))
    worst = frame_ms_desc[:count]
    return 1000.0 / (sum(worst) / len(worst))


def summarize(rows):
    frames = [r["frame_ms"] for r in rows if r["frame_ms"] > 0]
    if not frames:
        return None
    ordered = sorted(frames)
    descending = ordered[::-1]
    median = statistics.median(ordered)
    total_s = sum(frames) / 1000.0
    result = {
        "frames": len(frames),
        "seconds": total_s,
        "fps": len(frames) / total_s,
        "median_ms": median,
        "p95_ms": percentile(ordered, 0.95),
        "p99_ms": percentile(ordered, 0.99),
        "max_ms": ordered[-1],
        "low1_fps": low_fps(descending, 0.01),
        "low01_fps": low_fps(descending, 0.001),
        "over_2x_median": sum(1 for f in frames if f > 2 * median),
        "over_50ms": sum(1 for f in frames if f > 50),
        "over_100ms": sum(1 for f in frames if f > 100),
    }
    for column in ("present_wait_ms", "tick_wait_ms", "stage_a_cpu_ms", "draws", "compiles",
                   "alloc_mb", "free_mb", "protect_ms", "create_ms"):
        if all(column in r for r in rows):
            result[column + "_mean"] = statistics.fmean(r[column] for r in rows)
    result["compiles_total"] = sum(r.get("compiles", 0) for r in rows)
    result["alloc_mb_total"] = sum(r.get("alloc_mb", 0) for r in rows)
    result["free_mb_total"] = sum(r.get("free_mb", 0) for r in rows)
    return result


def verdict(s):
    frame = s["median_ms"]
    present = s.get("present_wait_ms_mean", 0.0)
    if frame <= 0:
        return "no data"
    share = present / frame
    if share > 0.35:
        return (f"GPU-bound: the frame preparation waits {share:.0%} of the frame time for the GPU. "
                "Lower the render cost (resolution/preset, BB_ANISO=4, effects) rather than the CPU's.")
    if share < 0.08:
        return (f"CPU-bound: the GPU is waited for only {share:.0%} of the frame. The game's code, the "
                "CPU compatibility layer or the command processing limit the frame rate; a lower "
                "resolution will not help.")
    return f"Mixed: {share:.0%} of the frame is spent waiting for the GPU."


def show(name, s):
    print(f"== {name}")
    if s is None:
        print("   no frames")
        return
    print(f"   {s['frames']} frames in {s['seconds']:.0f} s: {s['fps']:.1f} FPS average")
    print(f"   frame time: median {s['median_ms']:.2f} ms, p95 {s['p95_ms']:.2f}, p99 {s['p99_ms']:.2f}, "
          f"max {s['max_ms']:.1f}")
    print(f"   lows: 1% {s['low1_fps']:.1f} FPS, 0.1% {s['low01_fps']:.1f} FPS")
    print(f"   stutter: {s['over_2x_median']} frames over 2x the median, {s['over_50ms']} over 50 ms, "
          f"{s['over_100ms']} over 100 ms")
    for column in ("present_wait_ms", "tick_wait_ms", "stage_a_cpu_ms"):
        if column + "_mean" in s:
            print(f"   {column}: mean {s[column + '_mean']:.2f}")
    if "draws_mean" in s:
        print(f"   draws per frame: {s['draws_mean']:.0f}; shader compiles: {s['compiles_total']:.0f}; "
              f"device memory allocated {s['alloc_mb_total']:.0f} MB / freed {s['free_mb_total']:.0f} MB")
    print(f"   {verdict(s)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv")
    parser.add_argument("--compare", help="a second csv, printed after the first for an A/B comparison")
    parser.add_argument("--skip", type=float, default=20.0, help="seconds to skip at the start (default 20)")
    args = parser.parse_args()
    first = summarize(load(args.csv, args.skip))
    show(args.csv, first)
    if args.compare:
        second = summarize(load(args.compare, args.skip))
        show(args.compare, second)
        if first and second:
            print(f"== {args.compare} against {args.csv}: {second['fps'] - first['fps']:+.1f} FPS average, "
                  f"{second['low1_fps'] - first['low1_fps']:+.1f} FPS 1% low, "
                  f"{second['p99_ms'] - first['p99_ms']:+.2f} ms p99")
    return 0 if first else 1


if __name__ == "__main__":
    sys.exit(main())
