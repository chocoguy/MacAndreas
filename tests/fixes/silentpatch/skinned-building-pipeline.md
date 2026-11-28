# Skinned objects and the building pipeline (parachute)

Group `skinned-building-pipeline` (call_redirect at 0x5d7f1e).

**Bug.** Skinned objects loaded as buildings, like the parachute, get the building pipeline, which
can't skin them.

**Port.** As SilentPatch, `CCustomBuildingDNPipeline::CustomPipeAtomicSetup` is skipped for an
atomic whose pipeline is the skin plugin's (`rwID_SKINPLUGIN`, 0x116).

**Reproduction.** Not scripted yet (needs a parachute).

**Result.** Lifted and converted; not reached by the M9 route, which lists it.
