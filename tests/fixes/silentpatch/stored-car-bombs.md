# Bombs in stored cars and bikes

Group `stored-car-bombs` (call_redirects at 0x156d8ea and 0x15614ab, in the DRM-moved garage restore
loops of 0x448550 and 0x4485c0).

**Bug.** A car taken out of a garage gets its bomb back with no owner, and a bike loses its bomb.

**Port.** As SilentPatch, after `CStoredCar::RestoreCar`, a car or bike with a stored bomb type gets
the bomb (the low 3 bits at 0x4a8) and CJ as its owner (0x4cc).

**Reproduction.** Not scripted yet (needs a car with a bomb stored in a garage).

**Result.** Lifted and converted; not reached by the M9 route, which lists it.
