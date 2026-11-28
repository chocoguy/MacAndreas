# Streamed entity lists

Group `streamed-entity-lists` (byte_patches at 0x5b8e54 and 0x5b8eaf).

**Bug.** With high draw distance, the streamed entity list runs out and the world flickers when
looking down.

**Port.** The list holds 15000 entries instead of 12000, as SilentPatch.

**Reproduction.** Not scripted yet (needs maximum draw distance looking down from height).

**Result.** Lifted and converted.
