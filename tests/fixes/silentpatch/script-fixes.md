# Script fixes and gym glitch

Groups `script-fixes` (call_redirect after `CTheScripts::Load`, 0x5d18f0, and after the two
`WipeLocalVariableMemoryForMissionScript` calls, 0x489a70 and 0x4899f0) and `gym-glitch` (mid_hook on
GET_CURRENT_DATE at 0x470b03, resume 0x470b11). Setting `silentpatch.script_fixes` (on), replacing the INI's
`EnableScriptFixes`.

**Bugs.** The gym scripts compare the calendar date, so "You have worked out enough for today" can repeat
forever; saving in Madd Dogg's mansion loses the basketball; soft locks in Sweet's Girl and Mountain Cloud Boys;
the driving and bike schools delete random map objects as cones (blackboard glitch); Air Raid takes the
heavy weapon; Ice Cold Killa birds, Supply Lines and Beefy Baron teleport positions; the quadruple stunt bonus
compares a float height.

**Reproduction.** `tools/route/m9_silentpatch.py` starts a new game, which loads INITIAL (mission 0): the
basketball save pickup and point (seven values) move out of the court. With
`--set silentpatch.script_fixes=false` nothing is changed. The per-mission patches (missions 10, 18, 53, 61, 71,
72, 73, 120) match SilentPatch's byte patterns and are applied when those missions load; they are not reached
by a scripted route yet, and neither is the gym.

**Port.** The same byte patterns and replacements as SilentPatch, applied to guest script memory.

**Result.** Passed for INITIAL: `[SP] script: basketball save point moved (7 values)` and the quadruple stunt
bonus patch on New Game; with `silentpatch.script_fixes=false` neither happens. The per-mission fixes and the gym
glitch are not reached by a scripted route yet.
