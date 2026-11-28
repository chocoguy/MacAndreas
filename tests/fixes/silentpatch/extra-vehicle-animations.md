# Extra vehicle animations and engine components

Groups `extra-vehicle-animations` (vtable_redirects at 0x871164 and 0x87198c, call_redirects at the
three direct `CAutomobile::PreRender` calls) and `engine-components-off` (operand slots at 0x6ac2be
and 0x6acb91).

**Bug.** Several models have animated parts the game never moves, and the engine parts of the
Bandito, BF Injection and Hotknife spin with the engine off.

**Port.** As SilentPatch, with its stock SVF model list:
- Before `CAutomobile::PreRender`, the engine part speed is the time step only while the engine
  runs.
- After it, the Phoenix hood scoops flutter, the Sweeper brushes turn and the Newsvan antenna
  rotates.
- After `CPlane::PreRender`, the Beagle's and Stuntplane's extra flaps copy the aileron rotations.

SilentPatch swaps the vtable entries. The port does the same with the new `vtable_redirect` kind. The
matrix helpers (`SetComponentRotation` and SilentPatch's `CMatrix` multiply) are reimplemented on the
frames' modelling matrices.

**Reproduction.** Not scripted yet (needs those vehicles driven). The M9 route lists whether the
animations run.

**Result.** Lifted and converted.
