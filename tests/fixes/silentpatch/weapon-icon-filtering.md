# Weapon icon filtering

Group `weapon-icon-filtering` (byte_patch at 0x58d7d9).

**Bug.** Weapon icons are drawn with linear filtering and no mipmaps, so they shimmer when scaled down.

**Port.** As SilentPatch, the icon's texture filter is `rwFILTERMIPLINEAR`.

**Reproduction.** The weapon icon is on screen in every route; not compared against a reference.

**Result.** Lifted and converted; M4, M5, M7 and M8 pass.
