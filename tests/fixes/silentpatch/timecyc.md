# timecyc.dat fixes

Groups `timecyc-missing-data` (call_redirect on the line sscanf at 0x5bbce2) and `timecyc-float-multiplier`
(byte_patch at 0x5bbfc9).

**Bugs.** PC timecyc.dat has no directional multiplier column, so it is read from stack and truncated to an
integer; the RAINY_COUNTRYSIDE evening line is missing two ambient values, which tints it red.

**Port.** The multiplier defaults to 1.0 and stays a float; a line with three or more values missing is reparsed
with the broken line's format and keeps the previous line's ambient colour.

**Reproduction.** The route checks that exactly one line (RAINY_COUNTRYSIDE) is reparsed.

**Result.** Passed: exactly one line (20 of 52 values, RAINY_COUNTRYSIDE) is reparsed with its own format.
