#!/usr/bin/env python3
import argparse
import csv
import re
import subprocess
import sys
from pathlib import Path

from m4_route import GAME, Route

PLAYER_PED = 0xB7CD98
TIME_MS = 0xB7CB84
GET_ZONE_LOCKED = 0x572130
GIVE_WEAPON = 0x5E6080
AUDIO_TIME_MS = 0x4D9E80
REQUEST_MODEL = 0x4087E0
LOAD_ALL_REQUESTED = 0x40EA10
CREDITS_START = 0x7170E0
VEHICLE_CHEAT = 0x43A0B0
SET_GAME_CLOCK = 0x52D150
MOON_PEAK = (3, 40)
MOON_DAYS = 8
MOON_CAMERA = (2495.0, -1680.0, 80.0)
FIND_IPL_SLOT = 0x404AC0
IPL_RESOURCE_BASE = 25255
BEAGLE_IPL = "countn2_stream1"
HUNTER_MODEL = 425
TYPO_MODELS = (578, 558, 406)
CAMERA_WEAPON = 43
CAMERA_MODEL = 367
OUTSIDE_MAP = (9000.0, 9000.0)
GROVE = (2495.0, -1680.0)
VANILLA_OUTSIDE_BYTE = 0xBA3739 + 20 * 10 - 20
WINDOW = (100, 700)
REACHED = [
    ("corona flare scaling", "[SP] corona flares:"),
    ("lens flare buffer flush", "[SP] lens flare:"),
    ("deleted vehicles leave the impound list", "[SP] impound: deleted vehicle"),
    ("impound vehicle types", "[SP] impound: helicopters"),
    ("stencil shadow pads", "[SP] stencil shadows:"),
    ("mirror buffers", "[SP] mirrors:"),
    ("script sprites filtering", "[SP] script sprites:"),
    ("corona rotation", "[SP] coronas: rotation"),
    ("shadow alpha test", "[SP] shadows: static and stored"),
    ("16-bit script randoms", "[SP] script: random numbers are 16-bit"),
    ("plane spawn sectors", "[SP] planes: spawn check"),
    ("interior car generators", "[SP] car generator: vehicle above z 950"),
    ("post effects after a video mode change", "[SP] post effects: back buffer vertices"),
    ("underwater ripple frequency", "[SP] post effects: underwater ripple"),
    ("menu line wraps", "[SP] line wraps:"),
    ("map cursor limits", "[SP] map: LimitToMap"),
    ("crosshair sprites", "[SP] crosshairs:"),
    ("big message slide threshold", "[SP] big message:"),
    ("nitrous while reversing", "[SP] nitrous:"),
    ("jet thruster matrices", "[SP] jet thrusters:"),
    ("gang war friendly blips", "[SP] gang war:"),
    ("turf takeover text labels", "[SP] speech: turf takeover"),
    ("weather conversation replies", "[SP] speech: a positive reply to a weather"),
    ("gang taunts and attack lines", "[SP] speech: a gang member"),
    ("crash line fallback", "[SP] speech: a crash line"),
    ("PAIN_SPRAYED", "[SP] speech: a sprayed ped"),
    ("cop SURROUNDED lines", "[SP] speech: cops chasing"),
    ("Good Citizen bonus", "[SP] good citizen:"),
    ("shadows on rotated objects", "[SP] shadows: CastShadowEntityXY"),
    ("radio station name width", "[SP] radio:"),
    ("detached part colours", "[SP] detached parts: drawn"),
    ("lit flying components", "[SP] detached parts: flying components"),
    ("Phoenix, Sweeper and Newsvan animations", "[SP] vehicle animations:"),
    ("firela ladder", "[SP] firela:"),
    ("artict3 tow bar", "[SP] trailers:"),
    ("Tug tow bar", "[SP] tow bar:"),
    ("dirt material lists", "[SP] dirt: model info"),
    ("dirt remap", "[SP] dirt: "),
    ("unique number plates", "[SP] number plates: vehicle"),
    ("bike number plates", "[SP] number plates: a bike"),
    ("road reflections", "[SP] road reflections:"),
    ("light material restore", "[SP] lights:"),
    ("blown-up cars", "[SP] blown-up cars:"),
    ("light beam render states", "[SP] light beam:"),
    ("CAutomobile::Fix frame reset", "[SP] vehicle fix: CAutomobile::Fix"),
    ("CPlane::Fix panel reset", "[SP] vehicle fix: CPlane::Fix"),
    ("weapon in the ped's shadow", "[SP] shadows: a ped's weapon"),
    ("plane propeller alphas", "[SP] rotors: plane propeller alphas"),
    ("blended plane propellers", "[SP] rotors: plane propeller drawn"),
    ("_prop alpha callback", "[SP] rotors: a _prop atomic"),
    ("trailer dual rear wheels", "[SP] double rear wheels:"),
    ("loading screen loop", "[SP] loading screens:"),
    ("passenger hit-ped lines", "[SP] speech: a passenger comments"),
    ("no CAR_HIT_PED on pushes", "[SP] speech: pushing a ped"),
    ("New Game resets", "[SP] new game:"),
    ("paintjob remap fallback", "[SP] paintjobs:"),
    ("6 extra directionals", "[SP] lights: up to 6 extra directionals"),
    ("HUD float alpha", "[SP] HUD: the fading item"),
    ("radar sprite float alpha", "[SP] HUD: the radar sprite"),
    ("skinned atomics keep their pipeline", "[SP] pipelines: a skinned atomic"),
    ("radio change animation bones", "[SP] radio: the station change animation"),
    ("biker cop drive-by abort", "[SP] biker cops:"),
    ("racing checkpoint colours", "[SP] checkpoints:"),
    ("exploding car wheels", "[SP] exploding cars:"),
    ("stored car bombs", "[SP] garages: a stored car's bomb"),
]
SPAWNED = [
    ("the static rotor's alpha callback", "[SP] rotors: static_rotor gets the rotor alpha callback"),
    ("helicopter rotor alphas", "[SP] rotors: helicopter rotor alphas set"),
    ("blended helicopter rotors", "[SP] rotors: helicopter rotor drawn with blending"),
    ("the weapon drawn with the ped", "[SP] weapons: rendered with the ped"),
    ("the shadow camera update", "[SP] shadows: CShadowCamera::Update takes the entity"),
]
LOAD_TIME = [
    ("speech lookup tables", "[SP] speech: lookup tables patched"),
    ("Maccer's voice slot", "[SP] speech: Maccer gets the special voice slot"),
    ("WMYSGRD's voice", "[SP] speech: VOICE_GEN_WMYSGRAD found as VOICE_GEN_WMYSGRD"),
    ("BMYPIMP's voice", "[SP] speech: VOICE_GEN_BMYPI found as VOICE_GEN_BYMPI"),
    ("SBFYSTR's voice", "[SP] speech: SBFYSTR uses VOICE_GEN_SBFYSTR"),
    ("SWFYSTR's voice", "[SP] speech: SWFYSTR uses VOICE_GEN_SWFYSTR"),
    ("vehicle model info material lists", "[SP] model info: CVehicleModelInfo's material lists start empty"),
    ("handling.cfg names", "[SP] handling: names are matched whole"),
]


OPTIONAL_ON = ("silentpatch.coloured_zone_names", "silentpatch.directional_from_sun", "silentpatch.minimal_hud",
               "silentpatch.true_invincibility", "silentpatch.sliding_mission_title", "silentpatch.sliding_odd_job_text")
OPTIONAL_MARKERS = [
    ("sun reflections follow the time of day", "[SP] lights: the directional light follows the sun"),
    ("the minimal HUD mode", "[SP] minimal HUD: enabled in CHud::ReInitialise"),
]


def route(scale=1):
    r = Route()
    t = r.new_game()
    r.mark(t + WINDOW[0], "timing window start")
    r.mark(t + WINDOW[1], "timing window end")
    calls = [
        f"{t + 10}:0x{GET_ZONE_LOCKED:x}:cdecl:{OUTSIDE_MAP[0]:g}f,{OUTSIDE_MAP[1]:g}f",
        f"{t + 11}:0x{GET_ZONE_LOCKED:x}:cdecl:{GROVE[0]:g}f,{GROVE[1]:g}f",
        f"{t + WINDOW[0]}:0x{AUDIO_TIME_MS:x}:cdecl",
        f"{t + WINDOW[1]}:0x{AUDIO_TIME_MS:x}:cdecl",
        f"{t + 736}:0x{FIND_IPL_SLOT:x}:cdecl:${BEAGLE_IPL}",
        f"{t + 737}:0x{REQUEST_MODEL:x}:cdecl:@+{IPL_RESOURCE_BASE},2",
        f"{t + 738}:0x{REQUEST_MODEL:x}:cdecl:{HUNTER_MODEL},2",
        *(f"{t + 739}:0x{REQUEST_MODEL:x}:cdecl:{model},2" for model in TYPO_MODELS),
        f"{t + 740}:0x{REQUEST_MODEL:x}:cdecl:{CAMERA_MODEL},2",
        f"{t + 741}:0x{LOAD_ALL_REQUESTED:x}:cdecl:0",
        f"{t + 745}:0x{VEHICLE_CHEAT:x}:cdecl:{HUNTER_MODEL}",
        f"{t + 750}:0x{GIVE_WEAPON:x}:thiscall:*0x{PLAYER_PED:x},{CAMERA_WEAPON},50,1",
    ]
    dump = f"{t + 10}:{VANILLA_OUTSIDE_BYTE:x}:1:out/route9_vanilla_zone_byte.bin"
    first = t
    t += 770
    r.key(t, "e")
    r.mark(t + 20 * scale, "camera selected")
    r.hold(t + 40 * scale, 120 * scale, "rbutton")
    r.key(t + 100 * scale, "lbutton")
    r.mark(t + 140 * scale, "photo taken")
    calls.append(f"{t + 150 * scale}:0x{CREDITS_START:x}:cdecl")
    night = t + 152 * scale
    calls.append(f"{night}:0x{SET_GAME_CLOCK:x}:cdecl:{MOON_PEAK[0]},{MOON_PEAK[1]},0")
    calls += [f"{night + 1 + i}:0x{SET_GAME_CLOCK:x}:cdecl:{MOON_PEAK[0]},{MOON_PEAK[1]},1" for i in range(MOON_DAYS)]
    x, y, z = MOON_CAMERA
    r.pins = [f"{night + MOON_DAYS + 2}:{x:g},{y:g},{z:g}:{x:g},{y - 100:g},{z + 15:g}"]
    return r, t + 200 * scale, calls, dump, first, t + 190 * scale


def run(args, extra, log, csv_path, r, end, calls, dump, capture_dir, capture):
    Path(capture_dir).mkdir(parents=True, exist_ok=True)
    cmd = [args.host, "--game", args.game, "--native", args.native, "--manifest", "manifest", "--skip-intro",
           "--ignore-focus-loss", "--ignore-user-input", "--no-audio", "--keys", r.keys(),
           "--exit-after-frames", str(end), "--frame-times", csv_path, "--watch", f"{TIME_MS:x}",
           "--dump-memory", dump, "--capture-dir", capture_dir, "--capture-frames", str(capture)] + extra
    for c in calls:
        cmd += ["--call", c]
    for p in getattr(r, "pins", []):
        cmd += ["--pin-camera", p]
    with open(log, "w") as f:
        code = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT).returncode
    return code, Path(log).read_text(errors="replace")


def watch_series(text, addr):
    out = []
    for m in re.finditer(rf"\[WATCH\] frame (\d+) 0x{addr:08x} = 0x[0-9a-f]+ \((-?\d+)[,)]", text):
        out.append((int(m.group(1)), int(m.group(2))))
    return out


def at(series, frame):
    found = None
    for f, v in series:
        if f > frame:
            break
        found = v
    return found


def frame_ms(path, lo, hi):
    rows = []
    if not Path(path).exists():
        return rows
    with open(path) as f:
        for row in csv.DictReader(f):
            frame = int(row["frame"])
            if lo < frame <= hi:
                rows.append(float(row["ms"]))
    return rows


def timing(text, csv_path, first):
    lo, hi = first + WINDOW[0], first + WINDOW[1]
    frames = frame_ms(csv_path, lo, hi)
    series = watch_series(text, TIME_MS)
    g0, g1 = at(series, lo), at(series, hi)
    wall = sum(frames)
    game = None if g0 is None or g1 is None else g1 - g0
    mean = wall / len(frames) if frames else 0.0
    truncated = sum(int(ms) for ms in frames)
    return mean, wall, game, truncated


def calls_in(text):
    return re.findall(r"^\[CALL\] frame \d+: (\S+) = 0x([0-9a-f]+)", text, re.M)


def main():
    p = argparse.ArgumentParser(description="scripted M9 checks of the SilentPatch critical fixes: frame limiter and "
                                            "frame delay, timer fractions, zone bounds, script fixes, collision data, "
                                            "vehicles.ide defaults, the photo surface release, credits scaling and the Hunter door")
    p.add_argument("--host", default="build/dev/host/sa-host")
    p.add_argument("--native", default="build/dev/native/libgta_sa_native.dylib")
    p.add_argument("--game", default=GAME)
    p.add_argument("--set", action="append", default=[], help="extra --set KEY=VALUE for every run")
    args = p.parse_args()
    base = [x for kv in args.set for x in ("--set", kv)]
    Path("out").mkdir(exist_ok=True)
    ok = True
    runs = [
        ("30", ["--set", "silentpatch.frame_limit=30"], 1000 / 30, 1),
        ("60", ["--set", "silentpatch.frame_limit=60"], 1000 / 60, 2),
        ("uncapped, script fixes off, optional enhancements on",
         ["--set", "silentpatch.frame_limit=uncapped", "--set", "silentpatch.script_fixes=false",
          *[x for key in OPTIONAL_ON for x in ("--set", f"{key}=true")]], None, 4),
    ]
    for name, extra, target, scale in runs:
        r, end, calls, dump, first, capture = route(scale)
        tag = name.split(",")[0]
        log, csv_path = f"out/route9_{tag}.log", f"out/route9_{tag}.csv"
        code, text = run(args, base + extra, log, csv_path, r, end, calls, dump, f"out/route9_{tag}", capture)
        print(f"== frame limit {name}: sa-host exit {code}")
        ok = ok and code == 0
        for line in re.findall(r"^\[CRASH\].*", text, re.M):
            print(line)
        for line in re.findall(r"^\[SP\] (?:frame limiter|script|collision|vehicles\.ide: skimmer|photo|zones|cdstream).*", text, re.M):
            print(line)
        mean, wall, game, truncated = timing(text, csv_path, first)
        print(f"frames {first + WINDOW[0]}-{first + WINDOW[1]}: mean {mean:.2f} ms ({1000 / mean if mean else 0:.1f} fps), "
              f"wall {wall:.0f} ms, game clock {game} ms, whole-ms sum {truncated} ms")
        if target is not None:
            if abs(mean - target) / target < 0.05:
                print(f"ok: frames are paced at {1000 / target:.0f} fps")
            else:
                print(f"FAIL: mean frame time {mean:.2f} ms, expected {target:.2f}")
                ok = False
        elif mean >= 1000 / 31:
            print(f"FAIL: uncapped frames are no faster than 30 fps ({mean:.2f} ms)")
            ok = False
        else:
            print("ok: uncapped frames run faster than 30 fps")
        if game is not None and wall:
            ratio = game / wall
            loss = 1 - truncated / wall
            print(f"game clock / wall clock {ratio:.4f}; truncating each frame to whole ms would give {1 - loss:.4f}")
            if abs(ratio - 1) > 0.01:
                print("FAIL: the game clock drifts from real time by more than 1%")
                ok = False
            else:
                print("ok: game time keeps the fractional milliseconds")
        results = calls_in(text)
        outside = next((int(v, 16) for c, v in results if c.endswith(f"{OUTSIDE_MAP[0]:g}f,{OUTSIDE_MAP[1]:g}f")), None)
        grove = next((int(v, 16) for c, v in results if c.endswith(f"{GROVE[0]:g}f,{GROVE[1]:g}f")), None)
        vanilla = Path("out/route9_vanilla_zone_byte.bin")
        vanilla_byte = vanilla.read_bytes()[0] if vanilla.exists() else None
        print(f"zones: outside the map -> {outside}, Grove Street -> {grove}; vanilla would read byte "
              f"0x{VANILLA_OUTSIDE_BYTE:08x} = {vanilla_byte}, past the 10x10 visited-zones grid")
        if outside is None or (outside & 0xFF) != 1:
            print("FAIL: a position outside the map is not reported as unlocked")
            ok = False
        else:
            print("ok: outside the map counts as unlocked")
        scripts_off = "script_fixes=false" in " ".join(extra)
        mission0 = "[SP] script: mission 0 started with script fixes" in text
        basketball = re.search(r"\[SP\] script: basketball save point moved \((\d+) values\)", text)
        if scripts_off:
            if mission0 or basketball:
                print("FAIL: script fixes ran with silentpatch.script_fixes=false")
                ok = False
            else:
                print("ok: no script fixes with silentpatch.script_fixes=false")
        elif mission0 and basketball and basketball.group(1) == "7":
            print("ok: INITIAL got the basketball save point move (7 values)")
        else:
            print("FAIL: the INITIAL mission script was not fixed")
            ok = False
        optional = "optional enhancements on" in name
        for label, marker in OPTIONAL_MARKERS:
            if (marker in text) != optional:
                print(f"FAIL: {label} {'not reached with' if optional else 'reached without'} its setting")
                ok = False
            else:
                print(f"ok: {label} {'on' if optional else 'off'} with its setting")
        if "[SP] moon phases: 64x64 moon mask raster created" in text and "[SP] moon phases: the moon is masked by its phase" in text:
            print(f"ok: at {MOON_PEAK[0]}:{MOON_PEAK[1]:02d}, {MOON_DAYS} days on, the moon is drawn through the bundled mask")
        else:
            print("FAIL: the moon was not drawn through the moon phase mask")
            ok = False
        if "[SP] sun: drawn at a far clip of at most 600" in text:
            print("ok: the sun size hack places the sun at a far clip of at most 600")
        else:
            print("FAIL: the sun size hack was not reached")
            ok = False
        if "[SP] collision data:" not in text:
            print("FAIL: no CColData went through the initialising allocator")
            ok = False
        skimmer = re.search(r"\[SP\] vehicles\.ide: skimmer has (\d+) of 15 fields; wheel model -1", text, re.I)
        if skimmer:
            print("ok: the Skimmer line gets the wheel defaults instead of stale stack")
        else:
            print("FAIL: the Skimmer line was not seen with defaults")
            ok = False
        if scale != 1:
            continue
        photo = re.search(r"\[SP\] photo: back buffer surface 0x[0-9a-f]+ released, (\d+) references left", text)
        if photo:
            print(f"ok: the photo's back buffer surface is unlocked and released ({photo.group(1)} references left)")
        else:
            print("FAIL: no photo reached psGrabScreen's release")
            ok = False
        audio = [int(v, 16) for c, v in results if c.split(":")[1] == f"0x{AUDIO_TIME_MS:x}"]
        if len(audio) == 2 and wall:
            delta = audio[1] - audio[0]
            print(f"audio utility clock: {delta} ms over {wall:.0f} ms of frames")
            if abs(delta - wall) / wall < 0.01:
                print("ok: CAEAudioUtility::GetCurrentTimeInMs keeps real time")
            else:
                print("FAIL: the audio utility clock drifts from real time")
                ok = False
        else:
            print("FAIL: the audio utility clock was not sampled")
            ok = False
        reparsed = re.findall(r"^\[SP\] timecyc: line with (\d+) of 52 values reparsed", text, re.M)
        if len(reparsed) == 1:
            print(f"ok: one timecyc.dat line ({reparsed[0]} of 52 values, RAINY_COUNTRYSIDE) reparsed with its own format")
        else:
            print(f"FAIL: {len(reparsed)} timecyc.dat lines were reparsed, expected only RAINY_COUNTRYSIDE")
            ok = False
        if "[SP] anim: interpolator set up" in text:
            print("ok: animations start through the fixed RtAnimInterpolatorSetCurrentAnim")
        else:
            print("FAIL: no animation went through the fixed RtAnimInterpolatorSetCurrentAnim")
            ok = False
        if "[SP] credits: lines are placed on a fixed 448-unit screen height" in text and "[SP] credits: text scaled" in text:
            print(f"ok: the credits are placed in 448 units and printed scaled to the screen "
                  f"(capture: out/route9_{tag}/frame_{capture:05d}.png)")
        else:
            print("FAIL: the credits did not go through the scaling fix")
            ok = False
        if "[SP] car generator: the Fort Carson Beagle is a BMX" in text:
            print(f"ok: streaming {BEAGLE_IPL} turns the Fort Carson Beagle generator into a BMX")
        else:
            print(f"FAIL: streaming {BEAGLE_IPL} did not replace the Fort Carson Beagle")
            ok = False
        wrapped = re.search(r"\[SP\] vehicle animations: CAutomobile::PreRender wrapped \(site 0x([0-9a-f]+)", text)
        if wrapped:
            print(f"ok: CAutomobile::PreRender runs through its vtable_redirect (site 0x{wrapped.group(1)})")
        else:
            print("FAIL: CAutomobile::PreRender never went through the wrapper")
            ok = False
        typos = sorted(set(re.findall(r"^\[SP\] hierarchy: node (\S+) matches (\S+)", text, re.M)))
        if ("wheel_lm", "wheel_lm_dummy") in typos:
            print(f"ok: vehicle hierarchy typos accepted when the DFT-30, Uranus and Dumper load: {typos}")
        else:
            print(f"FAIL: the DFT-30's wheel_lm typo was not accepted ({typos})")
            ok = False
        if "[SP] hunter door: door_lf" in text:
            print("ok: the Hunter's door_lf is no longer marked as a left atomic")
        else:
            print("FAIL: loading the Hunter did not unmark door_lf")
            ok = False
        for name, marker in SPAWNED:
            if marker in text:
                print(f"ok: {name} (Hunter spawned next to CJ, camera in hand)")
            else:
                print(f"FAIL: {name} was not reached")
                ok = False
        for name, marker in LOAD_TIME:
            if marker in text:
                print(f"ok: {name} fixed at load")
            else:
                print(f"FAIL: {name} was not fixed at load")
                ok = False
        for name, marker in REACHED:
            print(f"{'reached' if marker in text else 'not reached'}: {name}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
