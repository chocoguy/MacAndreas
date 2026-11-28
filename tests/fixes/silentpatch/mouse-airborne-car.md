# Mouse rotating airborne vehicles

Group `mouse-airborne-car` (byte_patches at 0x6b4ebf and 0x6ce826).

**Bug.** The mouse rotates an airborne car or bike even when Steer with Mouse is off.

**Port.** The check reads `CVehicle::m_bEnableMouseSteering`, as SilentPatch.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted.
