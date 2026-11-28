# Rear light damage state

Group `rear-light-damage` (byte_patch at 0x6e1d4e).

**Bug.** Both rear lights read the damage state of the right tail light.

**Port.** As SilentPatch, the left one reads `LEFT_TAIL_LIGHT` (2).

**Reproduction.** Not scripted yet (needs a car with one rear light broken).

**Result.** Lifted and converted.
