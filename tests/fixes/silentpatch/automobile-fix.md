# Vehicle repair (CAutomobile::Fix, CPlane::Fix, FuckCarCompletely)

Groups `automobile-fix` (byte_patch at 0x6a34c9, mid_hook at 0x6a3555), `plane-fix` (mid_hook at
0x6cac05) and `fuck-car-completely-panels` (nop at 0x6c268d).

**Bug.** Repairing a vehicle zeroes its moving parts' rotations and resets every bouncing panel:
- Rumpo's bumper and Bandito's prop glitch.
- The Towtruck hitch breaks.
- The Rhino gains extra wheels.
- Plane doors corrupt.

Wrecking a car also zeroes its panel state, so a later repair doesn't fix the panels.

**Port.** As SilentPatch, with its stock SVF list:
- `CAutomobile::Fix` copies frames 8–24 from the model's own clump.
- It resets the bouncing panels except the Towtruck's and Tractor's hook.
- It resets the Rhino's extra wheels and hides them.
- `CPlane::Fix` resets the panels except the Vortex exhaust.
- `FuckCarCompletely` leaves the panel state.

SilentPatch rewrites `CAutomobile::Fix`'s epilogue to tail-jump to its own `Fix`; the port does that
work in a mid_hook, pops the four saved registers and resumes at the SEH epilogue.

**Reproduction.** Not scripted yet (needs a damaged vehicle repaired at a Pay 'n Spray or by a cheat).
The M9 route lists whether a fix runs.

**Result.** Lifted and converted.
