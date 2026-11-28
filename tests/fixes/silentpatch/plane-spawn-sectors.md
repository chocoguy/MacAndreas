# Plane spawning

Group `plane-spawn-sectors` (call_redirect at 0x6cd2b8).

**Bug.** `CPlane::FindPlaneCreationCoors` passes a position where the building collision check expects sector
numbers, so planes spawn where they crash.

**Port.** The position is converted with the game's sector constants (0x858b38, 0x858b34).

**Result.** Not reached by the New Game route (no plane spawns); no reproduction yet.
