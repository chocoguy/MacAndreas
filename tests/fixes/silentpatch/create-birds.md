# CREATE_BIRDS

Group `create-birds` (byte_patches at six `fild` instructions from 0x471967).

**Bug.** The script command reads its float coordinates as integers, so no birds spawn in the
skylight cutscene of 'Ice Cold Killa'.

**Port.** `fild` becomes `fld`, as SilentPatch.

**Reproduction.** Not scripted yet (needs the mission or a test script).

**Result.** Lifted and converted.
