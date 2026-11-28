# Muzzle flash from the last bullet

Group `muzzle-flash-last-bullet` (byte_patch at 0x61ecdc).

**Bug.** The flash is shown only when the clip still has ammo after the shot, so the last bullet has none.

**Port.** The check uses `CWeapon::Fire`'s result (al) instead.

**Reproduction.** Not scripted yet: needs a weapon with one bullet in the clip and a capture of the shot.

**Result.** Lifted and converted; no reproduction yet.
