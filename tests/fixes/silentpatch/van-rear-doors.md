# Rear van doors

Group `van-rear-doors` (byte_patch at 0x64e77b). Contributed to SilentPatch by B1ack_Wh1te.

**Bug.** `CCarEnterExit::GetPositionToOpenCarDoor` misses its van special case, so peds entering the
back of a van teleport slightly.

**Port.** The compare uses 13 (ANIM_VEH_VAN - ANIM_VEH_STD), as SilentPatch.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted.
