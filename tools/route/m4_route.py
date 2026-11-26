#!/usr/bin/env python3
import argparse
import re
import subprocess
import sys
from pathlib import Path

GAME = "gamefiles/Grand Theft Auto San Andreas"


class Route:
    def __init__(self):
        self.steps = []
        self.marks = []

    def key(self, frame, name, mode=""):
        self.steps.append((frame, f"{frame}:{name}{mode}"))

    def hold(self, frame, length, *names):
        for n in names:
            self.key(frame, n, "+")
            self.key(frame + length, n, "-")

    def type(self, frame, text, spacing=4):
        for i, c in enumerate(text.lower()):
            self.key(frame + i * spacing, c)
        return frame + len(text) * spacing

    def look(self, frame, dx, dy, count=1, spacing=2):
        for i in range(count):
            self.steps.append((frame + i * spacing, f"{frame + i * spacing}:look@{dx}:{dy}"))

    def mark(self, frame, label):
        self.marks.append((frame, label))

    def keys(self):
        return ",".join(s for _, s in sorted(self.steps, key=lambda x: x[0]))


def build(start):
    r = Route()
    r.key(1000, "return")
    r.key(1100, "return")
    t = start
    r.mark(t, "in control")
    t = r.type(t + 30, "hesoyam") + 30
    r.mark(t, "bike")
    r.key(t, "return")
    t += 90
    r.hold(t, 420, "w")
    for i in range(0, 420, 30):
        r.key(t + i, "space")
    r.hold(t + 150, 30, "d")
    t += 450
    r.key(t, "return")
    t += 90
    r.hold(t, 75, "w")
    t += 105
    r.mark(t, "car")
    t = r.type(t, "aiwprton") + 60
    r.key(t, "f")
    t += 180
    r.mark(t, "drive")
    r.hold(t, 900, "w")
    r.hold(t + 300, 60, "a")
    r.hold(t + 600, 45, "d")
    r.mark(t + 450, "mouse look")
    r.look(t + 450, 40, 0, count=30)
    r.look(t + 520, -40, 0, count=30)
    t += 960
    r.key(t, "f")
    t += 120
    r.mark(t, "walk")
    r.hold(t, 90, "w")
    t += 120
    r.mark(t, "sprint")
    r.hold(t, 150, "w", "space")
    t += 210
    r.mark(t, "fly")
    t = r.type(t, "rocketman") + 90
    r.hold(t, 300, "space")
    r.hold(t + 200, 600, "w")
    r.hold(t + 400, 60, "a")
    t += 900
    r.mark(t, "day and night")
    t = r.type(t, "ysohnul") + 30
    return r, t


def main():
    p = argparse.ArgumentParser(description="scripted M4 in-world route: walk, sprint, drive, bike, fly, day and night")
    p.add_argument("--host", default="build/dev/host/sa-host")
    p.add_argument("--native", default="build/dev/native/libgta_sa_native.dylib")
    p.add_argument("--game", default=GAME)
    p.add_argument("--start", type=int, default=6450, help="game frame at which the player has control")
    p.add_argument("--minutes", type=float, default=10.0, help="route length after the player has control")
    p.add_argument("--fps", type=float, default=30.0)
    p.add_argument("--capture-dir", default="out/route")
    p.add_argument("--capture-every", type=int, default=300)
    p.add_argument("--log", default="out/route.log")
    p.add_argument("--frame-times", default="out/route_frames.csv", help="per-frame times for tools/perf/frame_report.py")
    p.add_argument("--extra", default="", help="extra sa-host arguments, space separated")
    p.add_argument("--print-keys", action="store_true")
    args = p.parse_args()

    route, scripted_end = build(args.start)
    end = max(scripted_end + 60, args.start + int(args.minutes * 60 * args.fps))
    captures = sorted(set([f for f, _ in route.marks] + list(range(args.start, end + 1, args.capture_every)) + [end]))
    if args.print_keys:
        print(route.keys())
        return 0
    cmd = [args.host, "--game", args.game, "--native", args.native, "--manifest", "manifest", "--skip-intro",
           "--ignore-focus-loss", "--ignore-user-input", "--keys", route.keys(), "--capture-dir", args.capture_dir,
           "--capture-frames", ",".join(str(f) for f in captures), "--exit-after-frames", str(end),
           "--frame-times", args.frame_times] + args.extra.split()
    Path(args.capture_dir).mkdir(parents=True, exist_ok=True)
    Path(args.frame_times).parent.mkdir(parents=True, exist_ok=True)
    with open(args.log, "w") as log:
        code = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT).returncode
    text = Path(args.log).read_text(errors="replace")
    interp = sorted(set(re.findall(r"\[INTERP\] (0x[0-9a-f]+)", text)))
    for frame, label in route.marks:
        print(f"{frame:6d}  {label}")
    print(f"exit code {code}, {end} frames, {len(interp)} interpreted entry points")
    for a in interp:
        print(f"  [INTERP] {a}")
    perf = re.search(r"\[PERF\] (.*)", text)
    if perf:
        print(f"frame times: {perf.group(1)}")
    reached = re.search(r"exiting after (\d+) frames", text)
    return 0 if code == 0 and reached and not interp else 1


if __name__ == "__main__":
    sys.exit(main())
