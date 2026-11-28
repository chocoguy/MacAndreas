# Blown-up cars

Group `blown-up-cars` (call_redirect at 0x5d993f, mid_hooks at 0x5d9a74, 0x5d9b44 and 0x5d9cb2).

**Bug.** Blown-up vehicles render with wrong colours and shine.

**Port.** As SilentPatch (and 1.01): a wrecked car's atomic takes the dark path at the three
material branches when the directional light doesn't light atomics.

**Reproduction.** Not scripted yet (needs a blown-up car in view). The M9 route lists whether the dark
path is taken.

**Result.** Lifted and converted.
