# Zones fix

Group `zones-bounds` (func_replace `sp_zone_locked_or_unlocked` at `CTheZones::GetCurrentZoneLockedOrUnlocked`,
0x572130).

**Bug.** The function turns the position into a 10×10 grid index without a bounds check (and through a byte).
Far outside the map it reads past the visited-zones array, which gives the extra gang territories glitch and
breaks the Taxi Driver submission.

**Reproduction.** `sa-host --call` calls the function at (9000, 9000) and at Grove Street after New Game.
Vanilla indexes 10·20 − 20 + 9 = 189 into the array, i.e. byte 0xba37ed, which the route dumps.

**Port.** The grid constants are read from the original operands; out-of-range positions return unlocked.

**Result.** Passed: `GetCurrentZoneLockedOrUnlocked(9000, 9000)` returns 1 (unlocked); vanilla would read byte
0xba37ed, which holds 0 (locked) in this run. Grove Street returns 1, as the visited-zones grid holds.
