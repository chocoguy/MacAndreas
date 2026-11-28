# CJ clones in gang roadblocks

Group `roadblock-clones` (byte_patches at 0x4613c2 and 0x4613c6).

**Bug.** In 'End of the Line', some roadblocks in neutral zones have no gang and spawn CJ's model.

**Port.** The check compares the gang count with zero and spawns only when it is greater, as
SilentPatch.

**Reproduction.** Not scripted yet (needs 'End of the Line').

**Result.** Lifted and converted.
