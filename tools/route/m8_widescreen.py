#!/usr/bin/env python3
import argparse
import re
import subprocess
import sys
from pathlib import Path

from m4_route import GAME, Route

RATIOS = [("4:3", 960, 720), ("16:10", 1152, 720), ("16:9", 1280, 720), ("21:9", 1680, 720)]
VIEW = ((2234.2, -1262.1, 25.4), (2239.7, -1261.9, 24.4))
SCENE = (200, 90, 560, 410)
MENU_SIZE = (1680, 720)
MENU_FRAME = 1250
CUTSCENE_FRAME = 3000
LOADING_FRAME = 1240
MAP_ORIGIN_X = 0x00ba67b0
IMAGE_DIFF = Path(__file__).resolve().parents[1] / "verify" / "image_diff.py"
sys.path.insert(0, str(IMAGE_DIFF.parent))
from image_diff import read_png

HUD_PARTS = [("health bar", lambda r, g, b: r > 150 and g < 60 and b < 60),
             ("money", lambda r, g, b: g > 80 and r < g - 30 and b < g - 30)]


def vec(v):
    return ",".join(f"{x:g}" for x in v)


def hud_extent(path, test):
    img = read_png(path)
    xs, ys = [], []
    for y in range(0, img.height // 4):
        for x in range(img.width * 2 // 3, img.width):
            if test(*img.pixel(x, y)):
                xs.append(x)
                ys.append(y)
    if len(xs) < 20:
        return None
    return min(xs), max(xs) + 1 - min(xs), max(ys) + 1 - min(ys)


def dark_fraction(img, x0, x1):
    dark = total = 0
    for y in range(0, img.height, 2):
        for x in range(x0, x1, 2):
            r, g, b = img.pixel(x, y)
            total += 1
            dark += r < 12 and g < 12 and b < 12
    return dark / max(1, total)


def run_menu(args, extra, size=MENU_SIZE):
    from m7_silentpatch import menu_route
    r, end = menu_route()
    width, height = size
    out = Path(args.capture_dir) / f"menu-{width}x{height}"
    out.mkdir(parents=True, exist_ok=True)
    cmd = [args.host, "--game", args.game, "--native", args.native, "--manifest", "manifest", "--skip-intro",
           "--ignore-focus-loss", "--ignore-user-input", "--no-audio", "--resolution", f"{width}x{height}",
           "--keys", r.keys(), "--capture-dir", str(out), "--capture-frames", str(MENU_FRAME),
           "--exit-after-frames", str(MENU_FRAME + 10)] + extra
    log = out / "sa-host.log"
    with open(log, "w") as f:
        code = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT).returncode
    return code, out / f"frame_{MENU_FRAME:05d}.png"


def bright_extent(img):
    xs = [x for y in range(img.height) for x in range(img.width) if min(img.pixel(x, y)) > 200]
    if len(xs) < 20:
        return None
    xs.sort()
    return xs[len(xs) // 200], xs[-1 - len(xs) // 200]


def border_geometry(width, height):
    fraction = 0.7 + 8 / 480
    screen = width / height
    content = (4 / 3) / fraction
    if screen > content:
        return 0, 0, (width - height * content) / 2
    target = screen / content
    zoom = max(1.0, target + 1.5 * 18 * screen / 480 * 1.2 / 0.8)
    part = target / zoom
    center = 0.5 - 0.75 * 18 * screen / (480 * zoom)
    return max(center - part / 2, 0) * height, max(1 - center - part / 2, 0) * height, 0


def dark_run(img, rows, along):
    count = 0
    for i in rows:
        span = range(0, img.width, 2) if along == "row" else range(0, img.height, 2)
        dark = sum(1 for j in span if max(img.pixel(j, i) if along == "row" else img.pixel(i, j)) < 12)
        if dark < len(span) * 0.995:
            break
        count += 1
    return count


def run_cutscene(args, label, width, height, extra):
    r = Route()
    r.new_game(skip_cutscene=False)
    out = Path(args.capture_dir) / ("cutscene-" + label.replace(":", "x"))
    out.mkdir(parents=True, exist_ok=True)
    cmd = [args.host, "--game", args.game, "--native", args.native, "--manifest", "manifest", "--skip-intro",
           "--ignore-focus-loss", "--ignore-user-input", "--no-audio", "--resolution", f"{width}x{height}",
           "--keys", r.keys(), "--capture-dir", str(out), "--capture-frames", str(CUTSCENE_FRAME),
           "--exit-after-frames", str(CUTSCENE_FRAME + 10)] + extra
    with open(out / "sa-host.log", "w") as f:
        code = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT).returncode
    return code, out / f"frame_{CUTSCENE_FRAME:05d}.png"


def run_map(args, extra, framed):
    r = Route()
    t = r.new_game() + 60
    r.key(t, "escape")
    t += 80
    for i in range(2):
        r.key(t + i * 20, "down")
    t += 60
    r.key(t, "return")
    t += 120
    capture = t
    r.hold(t + 10, 90, "left")
    end = t + 110
    width, height = MENU_SIZE
    out = Path(args.capture_dir) / ("map-framed" if framed else "map")
    out.mkdir(parents=True, exist_ok=True)
    cmd = [args.host, "--game", args.game, "--native", args.native, "--manifest", "manifest", "--skip-intro",
           "--ignore-focus-loss", "--ignore-user-input", "--no-audio", "--resolution", f"{width}x{height}",
           "--keys", r.keys(), "--capture-dir", str(out), "--capture-frames", str(capture),
           "--watch", f"0x{MAP_ORIGIN_X:08x}", "--exit-after-frames", str(end)] + extra
    if framed:
        cmd += ["--set", "widescreen.fullscreen_map=false"]
    with open(out / "sa-host.log", "w") as f:
        code = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT).returncode
    text = (out / "sa-host.log").read_text(errors="replace")
    moves = [int(m) for m in re.findall(rf"^\[WATCH\] frame (\d+) 0x{MAP_ORIGIN_X:08x}", text, re.M)]
    return code, out / f"frame_{capture:05d}.png", sum(1 for f in moves if f > capture)


def run(args, label, width, height, extra):
    r = Route()
    t = r.new_game()
    pin, capture, end = t + 20, t + 50, t + 60
    out = Path(args.capture_dir) / label.replace(":", "x")
    out.mkdir(parents=True, exist_ok=True)
    cmd = [args.host, "--game", args.game, "--native", args.native, "--manifest", "manifest", "--skip-intro",
           "--ignore-focus-loss", "--ignore-user-input", "--no-audio", "--resolution", f"{width}x{height}",
           "--keys", r.keys(), "--pin-camera", f"{pin}:{vec(VIEW[0])}:{vec(VIEW[1])}",
           "--capture-dir", str(out), "--capture-frames", f"{LOADING_FRAME},{capture}",
           "--exit-after-frames", str(end)] + extra
    log = out / "sa-host.log"
    with open(log, "w") as f:
        code = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT).returncode
    return code, log.read_text(errors="replace"), out / f"frame_{capture:05d}.png"


def main():
    p = argparse.ArgumentParser(description="scripted M8 check: the game renders at each window's aspect ratio with a "
                                            "Hor+ field of view (the 4:3 picture is the middle of the wider ones)")
    p.add_argument("--host", default="build/dev/host/sa-host")
    p.add_argument("--native", default="build/dev/native/libgta_sa_native.dylib")
    p.add_argument("--game", default=GAME)
    p.add_argument("--capture-dir", default="out/route8")
    p.add_argument("--max-fraction", type=float, default=0.01, help="pixels allowed to differ in the compared scene")
    p.add_argument("--set", action="append", default=[], help="extra --set KEY=VALUE for sa-host")
    args = p.parse_args()
    extra = [x for kv in args.set for x in ("--set", kv)]
    fixed = "widescreen.aspect_fov=false" not in args.set
    hud = "widescreen.hud=false" not in args.set
    ok = True
    captures = {}
    for label, width, height in RATIOS:
        code, text, capture = run(args, label, width, height, extra)
        reported = re.search(r"^\[WS\] aspect ratio ([0-9.]+) \((\d+)x(\d+)\)", text, re.M)
        size = f"{width}x{height}"
        print(f"{label} {size}: sa-host exit {code}, " +
              (f"aspect ratio {reported.group(1)} for {reported.group(2)}x{reported.group(3)}" if reported
               else "no [WS] aspect ratio line"))
        if code != 0 or not capture.exists():
            print(f"FAIL: {label} run did not reach the capture")
            ok = False
            continue
        if fixed and (not reported or abs(float(reported.group(1)) - width / height) > 0.001):
            print(f"FAIL: {label} aspect ratio is not {width / height:.4f}")
            ok = False
        if not fixed and reported:
            print(f"FAIL: {label} reported a Widescreen Fix aspect ratio with the setting off")
            ok = False
        captures[label] = (width, capture)
        loading = capture.with_name(f"frame_{LOADING_FRAME:05d}.png")
        if loading.exists():
            img = read_png(loading)
            left = dark_run(img, range(img.width), "column")
            right = dark_run(img, range(img.width - 1, -1, -1), "column")
            bars = (width - height * 4 // 3) // 2 if hud else 0
            good = abs(left - bars) <= 2 and abs(right - bars) <= 2
            print(f"{label} loading screen: side bars {left}/{right} px, expected {bars}" + ("" if good else "  FAIL"))
            ok = ok and good
    base = captures.get("4:3")
    for label, (width, capture) in captures.items():
        if label == "4:3" or not base:
            continue
        x, y, w, h = SCENE
        region = f"{(width - base[0]) // 2 + x},{y},{w},{h}"
        result = subprocess.run([sys.executable, "-I", str(IMAGE_DIFF), str(capture), str(base[1]),
                                 "--region", region, "--reference-region", f"{x},{y},{w},{h}",
                                 "--max-fraction", str(args.max_fraction),
                                 "--diff", str(capture.with_name(capture.stem + "_vs_4x3.png"))],
                                capture_output=True, text=True)
        same = result.returncode == 0
        print(f"{label} middle vs 4:3: {result.stdout.strip() or result.stderr.strip()}")
        if same != fixed:
            print(f"FAIL: {label} middle should {'match' if fixed else 'differ from'} the 4:3 picture")
            ok = False
    hud_aspect = 0.0
    for kv in args.set:
        if kv.startswith("widescreen.hud_aspect=") and ":" in kv.split("=", 1)[1]:
            a, b = kv.split("=", 1)[1].split(":")
            hud_aspect = max(4 / 3, float(a) / float(b))
    for part, test in HUD_PARTS:
        sizes = {label: hud_extent(capture, test) for label, (width, capture) in captures.items()}
        if any(v is None for v in sizes.values()) or "4:3" not in sizes:
            print(f"FAIL: no {part} found in " + ", ".join(k for k, v in sizes.items() if v is None))
            ok = False
            continue
        base_x, base_w, base_h = sizes["4:3"]
        base_right = captures["4:3"][0] - base_x - base_w
        for label, (x, w, h) in sizes.items():
            width = captures[label][0]
            expected = base_w if hud else base_w * width / captures["4:3"][0]
            good = abs(w - expected) <= max(2, expected * 0.03) and abs(h - base_h) <= 2
            right = width - x - w
            if hud:
                inset = max(0.0, (width - 720 * hud_aspect) / 2) if hud_aspect else 0.0
                good = good and abs(right - (base_right + inset)) <= 2
            print(f"{label} {part}: {w}x{h} px, {right} px from the right edge, expected width {expected:.0f}" +
                  ("" if good else "  FAIL"))
            ok = ok and good
    canvas = hud and "widescreen.menu_aspect=auto" not in args.set
    code, capture = run_menu(args, extra)
    if code != 0 or not capture.exists():
        print("FAIL: the 21:9 menu run did not reach the capture")
        ok = False
    else:
        img = read_png(capture)
        bar = (img.width - img.height * 16 // 9) // 2
        sides = min(dark_fraction(img, 0, bar), dark_fraction(img, img.width - bar, img.width))
        inside = dark_fraction(img, img.width - bar - 200, img.width - bar)
        print(f"21:9 menu: {sides:.1%} of the {bar}-px side bars dark, {inside:.1%} of the canvas's right 200 px dark")
        if canvas and sides < 0.995:
            print("FAIL: the menu is not confined to a 16:9 canvas with black bars")
            ok = False
        if not canvas and sides > 0.9:
            print("FAIL: the menu leaves the sides dark without a menu canvas")
            ok = False
    if hud:
        extents = {}
        for size in ((1280, 1024), (640, 512)):
            code, capture = run_menu(args, extra, size)
            extents[size] = bright_extent(read_png(capture)) if code == 0 and capture.exists() else None
        full, small = extents[(1280, 1024)], extents[(640, 512)]
        if not full or not small:
            print("FAIL: the 1280x1024 or 640x512 menu run did not reach the capture")
            ok = False
        else:
            good = abs(small[0] * 2 - full[0]) <= 6 and abs(small[1] * 2 - full[1]) <= 6
            print(f"640x512 menu: text from x {small[0]} to {small[1]}, 1280x1024 from {full[0]} to {full[1]} "
                  f"(half: {full[0] / 2:.0f} to {full[1] / 2:.0f})" + ("" if good else "  FAIL"))
            ok = ok and good
    borders = fixed and not any(kv.startswith("widescreen.cutscene_borders=") for kv in args.set)
    for label, width, height in RATIOS if borders else []:
        if label == "16:10":
            continue
        code, capture = run_cutscene(args, label, width, height, extra)
        if code != 0 or not capture.exists():
            print(f"FAIL: the {label} cutscene run did not reach the capture")
            ok = False
            continue
        img = read_png(capture)
        top, bottom, side = border_geometry(width, height)
        got_top = dark_run(img, range(img.height), "row")
        got_bottom = dark_run(img, range(img.height - 1, -1, -1), "row")
        got_left = dark_run(img, range(img.width), "column")
        got_right = dark_run(img, range(img.width - 1, -1, -1), "column")
        good = (abs(got_left - side) <= 3 and abs(got_right - side) <= 3 and
                got_top >= top - 3 and got_bottom >= bottom - 3 and (side == 0 or got_top < 40))
        print(f"{label} cutscene: bars top {got_top}, bottom {got_bottom}, left {got_left}, right {got_right} px; "
              f"expected top {top:.0f}, bottom {bottom:.0f}, sides {side:.0f}" + ("" if good else "  FAIL"))
        ok = ok and good
    for framed in ([False, True] if hud else []):
        code, capture, moves = run_map(args, extra, framed)
        if code != 0 or not capture.exists():
            print(f"FAIL: the {'framed ' if framed else ''}map run did not reach the capture")
            ok = False
            continue
        img = read_png(capture)
        bar = (img.width - img.height * 16 // 9) // 2
        if framed:
            left = dark_run(img, range(img.width), "column")
            right = dark_run(img, range(img.width - 1, -1, -1), "column")
            good = left >= bar and right >= bar and moves >= 10
            print(f"21:9 framed map: dark columns {left}/{right} px (bars {bar}), map origin moved on {moves} frames" +
                  ("" if good else "  FAIL"))
        else:
            sides = max(dark_fraction(img, 0, bar), dark_fraction(img, img.width - bar, img.width))
            good = sides < 0.2 and moves >= 10
            print(f"21:9 full-screen map: {sides:.1%} of the side {bar} px dark, map origin moved on {moves} frames" +
                  ("" if good else "  FAIL"))
        ok = ok and good
    print("ok: Hor+ at every aspect ratio, HUD at 4:3 proportions" if ok and fixed and hud else
          "ok: Hor+ at every aspect ratio, HUD stretched as in vanilla" if ok and fixed else
          "ok: with the setting off the picture stretches, as in vanilla" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
