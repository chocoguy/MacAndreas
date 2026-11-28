# Second player's blip

Group `two-player-blips` (byte_patches at 0x588581, 0x56e2a4 and 0x56e2cb).

**Bug.** Both players' blips are drawn at player 1's position: `CRadar::DrawBlips` passes 0 to
`FindPlayerCentreOfWorld`, which also reads the player in focus instead of the index it was given.

**Port.** The player index is passed and used, as SilentPatch.

**Reproduction.** Not scripted yet (needs two-player mode).

**Result.** Lifted and converted.
