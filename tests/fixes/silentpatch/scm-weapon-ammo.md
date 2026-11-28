# Fixed ammo from SCM

Group `scm-weapon-ammo` (call_redirect at 0x47d335, GIVE_WEAPON_TO_CHAR's `CPed::GiveWeapon` call).

**Bug.** Scripts giving a weapon with 0 ammo leave an empty weapon that is dropped.

**Port.** 0 ammo becomes 1.

**Reproduction.** Not scripted yet (needs a mission that gives 0 ammo).

**Result.** Lifted and converted; no reproduction yet.
