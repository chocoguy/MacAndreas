# Hunter interior through the glass door

Group `hunter-door` (call_redirect at 0x4c9638).

**Bug.** The Hunter's left door is flagged as a left atomic, so the interior vanishes when viewed
through its glass from the right side.

**Port.** After `PreprocessHierarchy`, vehicles using the rustler animation block (doors that open
upwards) have `ATOMIC_IS_LEFT` cleared on `door_lf`. The clump's atomics are walked directly instead
of through `RpClumpForAllAtomics`.

**Reproduction.** The M9 route loads the Hunter (model 425) with `--call` and checks the unmark log.

**Result.** See the route output. The visual check (interior visible through the door) is not
scripted.
