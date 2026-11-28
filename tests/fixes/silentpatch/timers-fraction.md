# New timers fix

Group `timers-fraction` (call_redirect `sp_timer_ftol` on the four `_ftol` calls in `CTimer::Update` and the SCM
delta: 0x561c32, 0x561902, 0x56191a, 0x46a036).

**Bug.** Each frame's duration is truncated to whole milliseconds. Above 1000 fps every frame counts as 0 ms
and timers stop (the freezes on fade-outs with the limiter off); at lower rates game time runs slow by the lost
fractions (about 4% at 60 fps: 16.67 ms frames count as 16).

**Reproduction.** `tools/route/m9_silentpatch.py` compares `CTimer::m_snTimeInMilliseconds` (0xb7cb84) over
600 frames with the sum of the measured frame times, and prints what whole-millisecond truncation of the same
frames would give.

**Port.** Each call site keeps its own remainder and adds it to the next frame, as SilentPatch does.

**Result.** Passed (`tools/route/m9_silentpatch.py`, 600 frames): game clock / wall clock 1.0001 at 30 fps,
0.9992 at 60 fps and 0.9998 uncapped (120 fps). Truncating the same frames to whole milliseconds, as vanilla
does, would give 0.9848, 0.9649 and 0.9186.
