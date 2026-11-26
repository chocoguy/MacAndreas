#!/usr/bin/env python3
import argparse
import csv
import json
import math
import sys
from pathlib import Path

BUCKETS = [4, 8, 12, 16.7, 20, 25, 33.4, 50, 100, 250, 1000]


def load(path, skip):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    return [float(r["ms"]) for r in rows if int(r["frame"]) > skip]


def percentile(sorted_ms, p):
    if not sorted_ms:
        return 0.0
    return sorted_ms[min(len(sorted_ms) - 1, max(0, math.ceil(p * len(sorted_ms)) - 1))]


def stats(ms):
    s = sorted(ms)
    total = sum(s)
    return {
        "frames": len(s),
        "mean_ms": total / len(s) if s else 0.0,
        "p50_ms": percentile(s, 0.50),
        "p95_ms": percentile(s, 0.95),
        "p99_ms": percentile(s, 0.99),
        "max_ms": s[-1] if s else 0.0,
        "over_20ms": sum(1 for x in s if x > 20),
        "over_33ms": sum(1 for x in s if x > 33.4),
    }


def histogram(ms):
    counts = [0] * (len(BUCKETS) + 1)
    for x in ms:
        i = next((i for i, b in enumerate(BUCKETS) if x <= b), len(BUCKETS))
        counts[i] += 1
    return counts


def main():
    p = argparse.ArgumentParser(description="summarize sa-host --frame-times CSV files as a frame-time histogram")
    p.add_argument("csv", nargs="+")
    p.add_argument("--skip", type=int, default=0, help="ignore frames up to this number (loading, menus)")
    p.add_argument("--json", action="store_true", help="print one JSON object per file")
    args = p.parse_args()
    for path in args.csv:
        ms = load(path, args.skip)
        st = stats(ms)
        if args.json:
            print(json.dumps({"file": path, **st, "buckets_ms": BUCKETS, "histogram": histogram(ms)}))
            continue
        print(f"{Path(path).name}: {st['frames']} frames, mean {st['mean_ms']:.2f} ms "
              f"({1000 / st['mean_ms'] if st['mean_ms'] else 0:.1f} fps), p50 {st['p50_ms']:.2f}, "
              f"p95 {st['p95_ms']:.2f}, p99 {st['p99_ms']:.2f}, max {st['max_ms']:.1f} ms")
        counts = histogram(ms)
        peak = max(counts) or 1
        lo = 0
        for i, c in enumerate(counts):
            hi = BUCKETS[i] if i < len(BUCKETS) else None
            label = f"{lo:>6.1f}-{hi:<6.1f}" if hi else f"{lo:>6.1f}+      "
            bar = "#" * round(40 * c / peak)
            share = 100 * c / len(ms) if ms else 0
            print(f"  {label} ms {c:7d} {share:5.1f}% {bar}")
            lo = hi or lo
    return 0


if __name__ == "__main__":
    sys.exit(main())
