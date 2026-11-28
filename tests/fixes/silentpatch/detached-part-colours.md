# Detached vehicle parts

Groups `detached-part-colours` (mid_hook at 0x59f1ea) and `lit-flying-components` (call_redirect at
0x6a8bbe).

**Bug.** Parts that fall off a vehicle lose its colours and lighting, and their backfaces are culled.

**Port.** As SilentPatch:
- `CObject::Render` applies the vehicle's editable materials to a temporary car-part object
  (`SetEditableMaterialsCB`) and turns off backface culling.
- It then draws the object through `CEntity::Render` and restores both.
- Flying components get `bLightObject` before they are added to the world.

SilentPatch intercepts the tail jump to `CEntity::Render`; the port hooks the instruction before it
and calls `CEntity::Render` itself.

**Reproduction.** Not scripted yet (needs a damaged car losing a part). The M9 route lists whether
either hook is reached.

**Result.** Lifted and converted.
