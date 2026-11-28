# Roadblock law enforcement order

Group `roadblock-law-order` (byte_patches at 0x461ba7, 0x461bb0, 0x461bdd and 0x461be6).

**Bug.** `CRoadBlocks::GenerateRoadBlocks` checks for the army before SWAT, unlike the game's other
checks, so some roadblocks get Barracks instead of Enforcers.

**Port.** The two checks and their models swap places, as SilentPatch.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted.
