# Light material restore

Group `light-materials` (mid_hook at 0x4c830c).

**Bug.** Holding a weapon makes some objects lit wrongly, because only one of a material's
surface values is saved before lighting changes it.

**Port.** The two following surface values are saved for restore too, with the same register
results as SilentPatch's stub.

**Reproduction.** Not scripted yet. The M9 route lists whether the hook is reached.

**Result.** Lifted and converted.
