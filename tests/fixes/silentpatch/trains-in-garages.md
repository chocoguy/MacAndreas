# Allow to store trains in garages

Group `trains-in-garages` (call_redirect at 0x4480d0, the `CAutomobile` constructor call in `CStoredCar::RestoreCar`).

**Bug.** A train stored in a garage is restored as a `CAutomobile`, which crashes.

**Reproduction.** Not scripted yet (needs a train driven into a garage, or a crafted `CStoredCar`).

**Port.** Train models construct a `CTrain`, marked derailed, without infinite mass, and processing its own
collisions, as SilentPatch.

**Result.** Lifted and converted; no reproduction yet.
