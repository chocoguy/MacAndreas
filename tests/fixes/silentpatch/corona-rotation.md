# Corona rotation

Group `corona-rotation` (nop at 0x6fb277, call_redirect at 0x6fb2e6).

**Bug.** On PC, coronas don't rotate as the camera approaches, unlike the PS2.

**Port.** recipz is kept unscaled and the rotation argument is 20 × recipz, as SilentPatch.

**Reproduction.** Not scripted yet: needs two captures of a corona from different distances.

**Result.** Reached in the New Game route (the redirect runs for every rotated corona); the rotation itself is not compared yet.
