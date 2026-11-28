# Allow hovering on the Jetpack with Keyboard + Mouse controls

Group `jetpack-keyboard-hover` (mid_hook `sp_jetpack_keyboard_hover` at 0x67ed2d, resume 0x67edaf). Setting
`silentpatch.jetpack_keyboard_hover`.

**Bug.** Only a controller can hover the jetpack.

**Reproduction.** `tools/route/m7_silentpatch.py`: jetpack cheat, thrust held, Q+E (look behind) held, CJ's
height watched (`[0xB7CD98]+0x14>+0x38`).

**Result.** Passed (M7): the height holds at about 56.6 while Q+E is held. With the setting off CJ climbs about
100 units in the same window, as in vanilla.
