#!/usr/bin/env python3
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

from m4_route import GAME, Route

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "save"))
from sa_save import inspect  # noqa: E402

USER_FILES = Path.home() / "Library/Application Support/MacAndreas/Documents/GTA San Andreas User Files"
MONEY = 0xB7CE50
CUTSCENE_FRAMES = 5350


def save_route(slot):
    r = Route()
    r.key(1000, "return")
    r.key(1100, "return")
    t = 1100 + CUTSCENE_FRAMES + 100
    r.mark(t, "in control")
    r.key(t, f"save{slot}")
    r.mark(t + 50, "suicide cheat")
    r.type(t + 50, "szcmawo")
    t += 1750
    r.mark(t, "respawned, saving")
    for i in range(6):
        r.key(t + i * 300, f"save{slot}")
    return r, t + 1560


def load_route(slot):
    r = Route()
    r.key(1000, "return")
    r.key(1100, "down")
    r.key(1140, "return")
    t = 1200
    for _ in range(slot - 1):
        r.key(t, "down")
        t += 20
    r.key(t + 40, "return")
    r.key(t + 140, "return")
    r.mark(t + 140, "load confirmed")
    return r, t + 1800


def run(args, route, end, log, captures):
    cmd = [args.host, "--game", args.game, "--native", args.native, "--manifest", "manifest", "--skip-intro",
           "--ignore-focus-loss", "--ignore-user-input", "--no-audio", "--keys", route.keys(),
           "--capture-dir", args.capture_dir, "--capture-frames", ",".join(str(f) for f in captures),
           "--exit-after-frames", str(end), "--watch", f"{MONEY:x}"]
    with open(log, "w") as f:
        code = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT).returncode
    return code, Path(log).read_text(errors="replace")


def main():
    p = argparse.ArgumentParser(description="scripted M6 save/load check: fail the opening mission, save to a slot, "
                                            "restart, load the slot from the menu and check the player's money")
    p.add_argument("--host", default="build/dev/host/sa-host")
    p.add_argument("--native", default="build/dev/native/libgta_sa_native.dylib")
    p.add_argument("--game", default=GAME)
    p.add_argument("--slot", type=int, default=8, choices=range(1, 9))
    p.add_argument("--capture-dir", default="out/route6")
    p.add_argument("--keep", action="store_true", help="keep the save written by this run")
    args = p.parse_args()
    Path(args.capture_dir).mkdir(parents=True, exist_ok=True)
    save_file = USER_FILES / f"GTASAsf{args.slot}.b"
    backup = save_file.with_suffix(".b.route6-backup")
    if save_file.exists():
        shutil.copy2(save_file, backup)
    ok = True
    try:
        route, end = save_route(args.slot)
        code, text = run(args, route, end, "out/route6_save.log", [f for f, _ in route.marks] + [end])
        refused = "refused: a mission is running" in text
        saved = re.search(rf"\[SAVE\] slot {args.slot}: saved", text) is not None
        interp = sorted(set(re.findall(r"\[INTERP\] (0x[0-9a-f]+)", text)))
        print(f"save run: exit {code}, mission-time save refused: {refused}, saved after respawn: {saved}, "
              f"{len(interp)} interpreted entry points")
        problems, info, _ = inspect(save_file.read_bytes()) if save_file.exists() else (["no save file"], {}, 0)
        print(f"save file: {'ok' if not problems else '; '.join(problems)} {info}")
        ok = code == 0 and refused and saved and not problems and not interp
        route, end = load_route(args.slot)
        code, text = run(args, route, end, "out/route6_load.log", [1700, end])
        money = [int(v, 16) for v in re.findall(rf"\[WATCH\] frame \d+ 0x{MONEY:08x} = 0x([0-9a-f]+)", text)]
        interp = sorted(set(re.findall(r"\[INTERP\] (0x[0-9a-f]+)", text)))
        loaded = bool(money) and money[-1] == 250
        print(f"load run: exit {code}, money after load {money[-1] if money else '-'}, "
              f"{len(interp)} interpreted entry points")
        for a in interp:
            print(f"  [INTERP] {a}")
        ok = ok and code == 0 and loaded and not interp
    finally:
        if backup.exists():
            shutil.move(backup, save_file)
        elif not args.keep and save_file.exists():
            save_file.unlink()
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
