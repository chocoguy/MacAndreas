# Paint jobs kept in garages

Group `paintjob-remap-index` (func_replace of 0x6d0b70).

**Bug.** `CVehicle::GetRemapIndex` reads only the loaded remap. A car stored in a garage before it was
rendered has its remap still waiting to load, so the garage saves no paint job.

**Port.** As SilentPatch, the index falls back to the remap waiting to load.

**Reproduction.** Not scripted yet. The M9 route lists whether the fallback is used.

**Result.** Lifted and converted; the fallback is not reached on the route.
