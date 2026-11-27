#!/usr/bin/env python3
import argparse
import re
import subprocess
import sys
from pathlib import Path

from m4_route import GAME, Route

PLAYER_PED = 0xB7CD98
KNIFE_TYPE = 0x5BC
KNIFE_AMMO = 0x5C8
MATRIX = 0x14
POS_Z = 0x38
KNIFE = 4
VIEWS = [
    ("first control", (2234.2, -1262.1, 25.4), (2239.7, -1261.9, 24.4)),
]
CLOCK = "1170,10,90,35"


def route():
    r = Route()
    t = r.new_game()
    pins = []
    for name, pos, target in VIEWS:
        pins.append(f"{t + 20}:{vec(pos)}:{vec(target)}")
        r.mark(t + 50, f"view: {name}")
        pins.append(f"{t + 60}:restore")
        t += 100
    r.mark(t, "knife cheat")
    t = r.type(t, "professionalskit") + 200
    r.mark(t, "jetpack cheat")
    t = r.type(t, "rocketman") + 300
    r.mark(t, "take off")
    start = t
    t += 90
    r.mark(t, "hold q+e (look behind)")
    r.hold(t, 200, "q", "e")
    t += 260
    r.hold(start, t - start, "lshift")
    r.mark(t, "pause menu")
    r.key(t, "escape")
    t += 80
    for i in range(3):
        r.key(t + i * 20, "down")
    t += 80
    r.key(t, "return")
    t += 100
    r.mark(t, "stats")
    for i in range(7):
        r.key(t + i * 20, "down")
    t += 240
    r.mark(t, "misc stats")
    return r, t + 40, pins


def menu_route():
    r = Route()
    r.key(1000, "down")
    r.key(1040, "return")
    r.mark(1100, "options")
    r.key(1140, "down")
    r.key(1160, "down")
    r.key(1200, "return")
    r.mark(1250, "display setup")
    for i in range(6):
        r.key(1300 + i * 20, "down")
    r.key(1440, "return")
    r.mark(1540, "advanced display options")
    return r, 1580


def vec(v):
    return ",".join(f"{x:g}" for x in v)


def chain(*parts):
    return ">".join(f"{p:x}" for p in parts)


def run(args, extra, log, r, end, capture_dir, watch=True, pins=()):
    cmd = [args.host, "--game", args.game, "--native", args.native, "--manifest", "manifest", "--skip-intro",
           "--ignore-focus-loss", "--ignore-user-input", "--no-audio", "--keys", r.keys(),
           "--exit-after-frames", str(end), "--capture-dir", capture_dir,
           "--capture-frames", ",".join(str(f) for f, _ in r.marks)] + extra
    for pin in pins:
        cmd += ["--pin-camera", pin]
    if watch:
        cmd += ["--watch", "b6ec2e", "--watch-chain", ",".join([chain(PLAYER_PED, KNIFE_TYPE),
                chain(PLAYER_PED, KNIFE_AMMO), chain(PLAYER_PED, MATRIX, POS_Z)])]
    Path(capture_dir).mkdir(parents=True, exist_ok=True)
    with open(log, "w") as f:
        code = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT).returncode
    return code, Path(log).read_text(errors="replace")


def values(text, name):
    out = []
    for m in re.finditer(rf"\[WATCH\] frame (\d+) {re.escape(name)} = 0x[0-9a-f]+ \(-?\d+, ([^)]+)\)", text):
        out.append((int(m.group(1)), float(m.group(2))))
    return out


def value_at(series, frame):
    found = None
    for f, v in series:
        if f > frame:
            break
        found = v
    return found


def last_value(text, name):
    found = None
    for m in re.finditer(rf"\[WATCH\] frame (\d+) {re.escape(name)} = 0x([0-9a-f]+) \((-?\d+), ([^)]+)\)", text):
        found = (int(m.group(1)), int(m.group(3)), float(m.group(4)))
    return found


def main():
    p = argparse.ArgumentParser(description="scripted M7 check of the first SilentPatch ports: melee cheat ammo "
                                            "(byte_patch), units (func_replace), jetpack keyboard hover (mid_hook) "
                                            "and the resolution string fallback (call_redirect)")
    p.add_argument("--host", default="build/dev/host/sa-host")
    p.add_argument("--native", default="build/dev/native/libgta_sa_native.dylib")
    p.add_argument("--game", default=GAME)
    p.add_argument("--capture-dir", default="out/route7")
    p.add_argument("--set", action="append", default=[], help="extra --set KEY=VALUE for sa-host")
    p.add_argument("--reference", help="directory of captures to compare the pinned views against")
    p.add_argument("--max-fraction", type=float, default=0.005, help="pixels allowed to differ in a pinned view")
    args = p.parse_args()
    extra = [x for kv in args.set for x in ("--set", kv)]
    menu, menu_end = menu_route()
    menu_code, menu_text = run(args, extra, "out/route7_menu.log", menu, menu_end, args.capture_dir + "/menu", False)
    print(f"sa-host exit {menu_code} (menu run)")
    ok = menu_code == 0
    reached = re.search(r"^\[SP\] resolution string: redirect reached at (0x[0-9a-f]+) .*", menu_text, re.M)
    if reached and reached.group(1) == "0x0057a071":
        print(reached.group(0))
        for line in re.findall(r"^\[SP\] resolution string: missing.*", menu_text, re.M):
            print(line)
        print("ok: Advanced Display Options goes through the call_redirect and back to AsciiToGxtChar")
    else:
        print("FAIL: the resolution string call never reached the call_redirect")
        ok = False
    r, end, pins = route()
    code, text = run(args, extra, "out/route7.log", r, end, args.capture_dir, pins=pins)
    for line in re.findall(r"^\[CAMERA\].*", text, re.M):
        print(line)
    views = [(f, label) for f, label in r.marks if label.startswith("view: ")]
    if text.count("[CAMERA]") != 2 * len(views):
        print("FAIL: the camera was not pinned and restored for every view")
        ok = False
    for f, label in views:
        capture = Path(args.capture_dir) / f"frame_{f:05d}.png"
        if args.reference:
            ref = Path(args.reference) / capture.name
            diff = [sys.executable, "-I", str(Path(__file__).resolve().parents[1] / "verify" / "image_diff.py"),
                    str(capture), str(ref), "--ignore", CLOCK, "--max-fraction", str(args.max_fraction),
                    "--diff", str(capture.with_name(capture.stem + "_diff.png"))]
            result = subprocess.run(diff, capture_output=True, text=True)
            print(f"{label}: {result.stdout.strip() or result.stderr.strip()}")
            ok = ok and result.returncode == 0
        else:
            print(f"{label}: pinned capture {capture}")
    knife = last_value(text, f"0x{PLAYER_PED:x}>{KNIFE_TYPE:x}")
    ammo = last_value(text, f"0x{PLAYER_PED:x}>{KNIFE_AMMO:x}")
    print(f"sa-host exit {code}")
    for line in re.findall(r"^\[MANIFEST\].*|^sa-host: manifest:.*|^\[SP\].*", text, re.M):
        print(line)
    if not knife or knife[1] != KNIFE:
        print(f"FAIL: melee slot holds weapon {knife and knife[1]}, expected the knife ({KNIFE})")
        ok = False
    elif not ammo or ammo[1] != 1:
        print(f"FAIL: knife total ammo is {ammo and ammo[1]}, expected 1 (vanilla gives 0)")
        ok = False
    else:
        print(f"ok: knife given with 1 ammo (frame {ammo[0]})")
    hover_on = "silentpatch.jetpack_keyboard_hover=false" not in args.set
    hold = next(f for f, label in r.marks if label.startswith("hold q+e"))
    z = values(text, f"0x{PLAYER_PED:x}>{MATRIX:x}>{POS_Z:x}")
    z0, z1 = value_at(z, hold + 40), value_at(z, hold + 190)
    climb = None if z0 is None or z1 is None else z1 - z0
    hovered = climb is not None and abs(climb) < 2.0
    print(f"jetpack: height {z0} -> {z1} while look-behind is held (thrust held throughout)")
    if hover_on and hovered and "[SP] jetpack: look-behind hovers" in text:
        print("ok: look-behind hovers with keyboard controls")
    elif not hover_on and climb is not None and climb > 10 and "[SP] jetpack" not in text:
        print("ok: with the setting off the hook is not registered and the jetpack keeps climbing (vanilla)")
    else:
        print("FAIL: jetpack hover does not match silentpatch.jetpack_keyboard_hover")
        ok = False
    units = re.search(r"\[SP\] units: (\w+)", text)
    if units:
        print(f"ok: units function replaced, answered {units.group(1)}")
    else:
        print("note: the game did not ask for units during this route")
    print("marks:", ", ".join(f"{f} {label}" for f, label in menu.marks + r.marks))
    return 0 if ok and code == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
