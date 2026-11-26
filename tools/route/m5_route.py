#!/usr/bin/env python3
import argparse
import array
import math
import re
import subprocess
import sys
import wave
from pathlib import Path

from m4_route import GAME, Route

STATIONS = ["CH", "CO", "CR", "DS", "HC", "MH", "MR", "NJ", "RE", "RG", "TK"]
CUTSCENE_FRAMES = 5350


def build(station_spacing):
    r = Route()
    r.key(1000, "down")
    r.key(1040, "return")
    r.key(1100, "down")
    r.key(1140, "return")
    for i in range(5):
        r.key(1240 + i * 30, "down")
    t = 1500
    r.mark(t, "radio stations")
    for i in range(len(STATIONS) + 2):
        r.key(t + i * station_spacing, "right")
    t += (len(STATIONS) + 2) * station_spacing
    r.mark(t, "back to main menu")
    r.key(t, "escape")
    r.key(t + 50, "escape")
    r.key(t + 100, "up")
    r.key(t + 150, "return")
    r.key(t + 250, "return")
    t += 250
    r.mark(t + 60, "intro cutscene")
    t += CUTSCENE_FRAMES
    r.mark(t, "in control")
    t = r.type(t + 30, "hesoyam") + 30
    r.key(t, "return")
    t += 90
    r.mark(t, "bike and mission dialogue")
    r.hold(t, 420, "w")
    for i in range(0, 420, 30):
        r.key(t + i, "space")
    r.hold(t + 150, 30, "d")
    t += 450
    r.key(t, "return")
    t += 90
    r.mark(t, "walk")
    r.hold(t, 90, "w")
    t += 150
    return r, t


def loudness(path):
    with wave.open(str(path)) as w:
        rate = w.getframerate()
        data = array.array("h", w.readframes(w.getnframes()))
    levels = []
    for s in range(len(data) // (2 * rate)):
        seg = data[2 * s * rate:2 * (s + 1) * rate]
        rms = math.sqrt(sum(x * x for x in seg) / max(1, len(seg))) / 32768
        levels.append(20 * math.log10(rms + 1e-9))
    return levels


def main():
    p = argparse.ArgumentParser(description="scripted M5 audio route: every radio station, the intro cutscene, "
                                            "mission dialogue and SFX, captured to a WAV")
    p.add_argument("--host", default="build/dev/host/sa-host")
    p.add_argument("--native", default="build/dev/native/libgta_sa_native.dylib")
    p.add_argument("--game", default=GAME)
    p.add_argument("--station-spacing", type=int, default=300, help="frames between radio station changes")
    p.add_argument("--capture-dir", default="out/route5")
    p.add_argument("--log", default="out/route5.log")
    p.add_argument("--audio", default="out/route5.wav")
    p.add_argument("--speakers", action="store_true", help="play through the output device instead of muting it")
    p.add_argument("--print-keys", action="store_true")
    args = p.parse_args()

    route, end = build(args.station_spacing)
    if args.print_keys:
        print(route.keys())
        return 0
    captures = sorted(set([f for f, _ in route.marks] + [f + 120 for f, _ in route.marks] + [end]))
    cmd = [args.host, "--game", args.game, "--native", args.native, "--manifest", "manifest", "--skip-intro",
           "--ignore-focus-loss", "--ignore-user-input", "--keys", route.keys(), "--capture-dir", args.capture_dir,
           "--capture-frames", ",".join(str(f) for f in captures), "--exit-after-frames", str(end),
           "--audio-capture", args.audio]
    if not args.speakers:
        cmd.append("--no-audio")
    Path(args.capture_dir).mkdir(parents=True, exist_ok=True)
    Path(args.audio).parent.mkdir(parents=True, exist_ok=True)
    with open(args.log, "w") as log:
        code = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT).returncode
    text = Path(args.log).read_text(errors="replace")
    interp = sorted(set(re.findall(r"\[INTERP\] (0x[0-9a-f]+)", text)))
    streams = sorted(set(re.findall(r'\[FILE\] open "AUDIO\\STREAMS\\(\w+)"', text, re.IGNORECASE)))
    missing = [s for s in STATIONS + ["CUTSCENE"] if s not in streams]
    for frame, label in route.marks:
        print(f"{frame:6d}  {label}")
    print(f"exit code {code}, {end} frames, {len(interp)} interpreted entry points")
    for a in interp:
        print(f"  [INTERP] {a}")
    print(f"streams opened: {' '.join(streams)}")
    if missing:
        print(f"streams never opened: {' '.join(missing)}")
    levels = loudness(args.audio) if Path(args.audio).exists() else []
    audible = [i for i, db in enumerate(levels) if db > -60]
    silent_after = sum(1 for db in levels[audible[0]:] if db <= -60) if audible else 0
    print(f"audio: {len(levels)} s captured, first sound at {audible[0] if audible else '-'} s, "
          f"{silent_after} silent seconds after that")
    ok_audio = bool(audible) and silent_after <= len(levels) // 10
    reached = re.search(r"exiting after (\d+) frames", text)
    return 0 if code == 0 and reached and not interp and not missing and ok_audio else 1


if __name__ == "__main__":
    sys.exit(main())
