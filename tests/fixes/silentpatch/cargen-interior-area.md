# Car generators placed in interiors

Group `cargen-interior-area` (call_redirect at 0x6f3b30, the generator's SetPosition call).

**Bug.** Generators above z 950 (interiors) spawn their vehicle in area 0, so the two cars in Madd Dogg's mansion
are never visible inside it.

**Port.** After the position is set, z ≥ 950 sets the entity's area code (+0x2f) to 13, as SilentPatch.

**Reproduction.** Not scripted yet: needs a save inside Madd Dogg's mansion. The route reports whether the hook
places a vehicle.

**Result.** Lifted and converted; no reproduction yet.
