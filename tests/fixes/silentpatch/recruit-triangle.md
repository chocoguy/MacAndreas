# Fixed triangle above recruitable peds' heads

Group `recruit-triangle` (byte_patch at 0x60bc52: `cmp eax, 8`).

**Bug.** The aim triangle is upside down only up to GANG1, so recruitable GANG2 (Grove) members show the normal one.

**Reproduction.** Not scripted yet (needs aiming at a Grove member).

**Result.** Lifted and converted; no reproduction yet.
