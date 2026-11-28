# Biker cops' drive-by after losing the wanted level

Group `biker-cop-drive-by` (call_redirect at 0x1561366, in the DRM-moved
`CCarAI::BackToCruisingIfNoWantedLevel`).

**Bug.** Biker cops keep shooting at CJ after his wanted level is gone.

**Port.** As SilentPatch, after `JoinCarWithRoadSystem`, if the driver's primary task is a sequence
(244) containing `TASK_SIMPLE_GANG_DRIVEBY` (1022), the sequence is made abortable. Task types and
`MakeAbortable` are called through the tasks' vtables.

**Reproduction.** Not scripted yet (needs a wanted level with biker cops, then losing it).

**Result.** Lifted and converted; not reached by the M9 route, which lists it.
