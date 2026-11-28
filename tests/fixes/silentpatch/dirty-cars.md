# Dirty cars getting clean

Group `dirty-cars` (mid_hook at 0x4c9648, func_replace at 0x5d5db0, vtable_redirect at 0x85c5e4).

**Bug.** Only the first 16 dirt materials are listed and remapped, so many cars never get clean.

**Port.** As SilentPatch:
- Every `vehiclegrunge256` material of the clump and its extras is listed: 30 in the model info and
  the rest in a list kept on the host side. SilentPatch allocates those with `new[]`.
- `RemapDirt` sets the dirt texture on all of them.
- `CVehicleModelInfo::Shutdown` drops the list.

**Reproduction.** Partial: the M9 route lists whether models get their dirt lists and whether dirt is
remapped.

**Result.** See the route's reached list.
