# Weapon icon on a jetpack

Group `weapon-icon-zwrite` (nops at 0x58e210, 0x58eab7 and 0x58eae1).

**Bug.** The crosshair turns z writes off, which breaks the weapon icon while on a jetpack.

**Port.** The three `RwRenderStateSet(ZWRITEENABLE)` calls are skipped, as SilentPatch.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted.
