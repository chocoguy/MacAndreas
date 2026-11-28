# Heap corruption fix

Group `heap-corruption` (nop, 5 bytes at 0x5c25d3, the call to 0x4a9cd0), as SilentPatch.

**Bug.** SilentPatch reports a heap corruption from this call that does not affect gameplay.

**Reproduction.** Not scripted: the corruption has no visible effect. Pending a heap check (guarded allocator) run.

**Result.** Lifted and converted; no reproduction yet.
