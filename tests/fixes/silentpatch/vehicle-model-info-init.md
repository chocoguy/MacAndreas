# CVehicleModelInfo initialisation

Group `vehicle-model-info-init` (call_redirect at 0x4c7633).

**Bug.** `CVehicleModelInfo`'s constructor leaves its colour and dirt material lists uninitialised.

**Port.** As SilentPatch, after the `CClumpModelInfo` base constructor:
- The four colour material lists and the in-place dirt list are cleared.
- The dirt list pointer is null and its count is 0.
- The removed per-model plate material points at a dummy, so the null checks still in the game pass;
  plates are per vehicle (`number-plates.md`).

**Reproduction.** Runs for every vehicle model at startup.

**Result.** Passed: `[SP] model info: CVehicleModelInfo's material lists start empty` at load in every M9 run.
