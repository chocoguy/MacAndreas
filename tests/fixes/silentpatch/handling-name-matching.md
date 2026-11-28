# handling.cfg name matching

Group `handling-name-matching` (call_redirects at 0x6f4f58 and 0x6f4f64).

**Bug.** `CHandlingDataMgr::FindExactWord` compares only as many characters as the line's word, so a
handling name that is a prefix of another matches the wrong entry.

**Port.** As SilentPatch, the word is kept as a pointer instead of copied, and the names are compared
whole with `strcmp`.

**Reproduction.** Runs for every handling.cfg line at startup.

**Result.** Passed: `[SP] handling: names are matched whole` at load in every M9 run; vehicles load and drive in M4.
