# Make freeing temp objects more aggressive to fix vending crash

Group `temp-objects-vending` (func_replace at `CObject::TryToFreeUpTempObjects`, 0x5a1840).

**Bug.** Using a vending machine for a long time fills the object pool with temp objects (cans); the game only
frees invisible ones and crashes when the pool is full.

**Reproduction.** Not scripted yet (needs a vending machine and a few hundred uses, or a `--call` with a full pool).

**Port.** Free invisible temp objects first, then visible ones up to the number requested, as SilentPatch.

**Result.** Lifted and converted; no reproduction yet.
