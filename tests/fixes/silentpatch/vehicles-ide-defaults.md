# Fix Skimmer not spawning correctly

Group `vehicles-ide-defaults` (call_redirect on `CFileLoader::LoadVehicleObject`'s sscanf, 0x5b6fc7).

**Bug.** Some vehicles.ide lines (the Skimmer's) lack the wheel model, wheel sizes and upgrade class. sscanf
leaves those variables as stale stack, so the Skimmer gets garbage wheel data and shoots into the sky. Windows 11
24H2 exposed it; the lifted code's stack differs from Windows, so the port is exposed too.

**Reproduction.** The redirect logs every line with fewer than 17 fields and the values used.

**Port.** The four outputs get SilentPatch's defaults (-1, 0.7, 0.7, -1) before the call.

**Result.** Passed: sscanf assigns at most 15 fields. The Skimmer line has 11 and gets wheel model -1, sizes
0.70/0.70 and upgrade class -1 instead of stack contents. The ten boats (coastg, dinghy, jetmax, launch, marquis,
predator, reefer, speeder, squalo, tropic) have 11 fields too and get the same defaults; every other vehicle line
has all 15.
