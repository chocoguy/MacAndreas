# Cops holding one-handed guns

Group `cop-one-handed-guns` (byte_patches at 0x61e52e, 0x61e533, 0x625434 and 0x625439).
Contributed to SilentPatch by iFarbod.

**Bug.** Cops hold one-handed weapons tilted, like gangsters.

**Port.** The ped type checks cover gangs and dealers (up to 17) and criminals (20) instead of gangs
and cops (6), as SilentPatch.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted.
