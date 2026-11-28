# Frame limiter and frame delay

Groups `no-frame-delay` (silentpatch, byte_patch and nop; triage "No framedelay", keep) and `frame-limiter`
(native mid_hook `sp_frame_pacing` at 0x748d1f; triage "(Hopefully) more precise frame limiter", replace).
Setting `silentpatch.frame_limit`: 30 (default), 60 or uncapped.

**Bug.** `Idle` busy-waits until 14 ms have passed since the previous frame before the frame limiter's own
wait, so with the Frame Limiter on the game runs at 25 fps instead of 30. The limiter itself spins the main loop
on a millisecond timer.

**Reproduction.** Before M9 the M4 route (Frame Limiter on) measured mean frame times of 38.6–42.5 ms, i.e.
about 25 fps (ROADMAP M8 results). `tools/route/m9_silentpatch.py` measures frames 100–700 after first control
with `--frame-times`.

**Port.** SilentPatch's three byte patches skip the 14 ms wait. The main loop's limiter test keeps vanilla's
conditions (menu Frame Limiter, the forced limit at 0xa43088, the audio engine's check) and then sleeps on the
host clock until the next frame at `silentpatch.frame_limit`, instead of spinning. Uncapped leaves only the
display's refresh; with the menu limiter off the game is uncapped, as in vanilla.

**Result.** Passed (`tools/route/m9_silentpatch.py`, frames 100–700 after first control):
- `silentpatch.frame_limit=30`: mean 33.33 ms (30.0 fps), against about 40 ms (25 fps) before M9.
- `=60`: mean 16.67 ms (60.0 fps).
- `=uncapped`: mean 8.33 ms (120 fps, this Mac's display refresh).
