# Crash comments on a motorbike

Group `crash-generic-fallback` (call_redirects at 0x6a8254 and 0x6b9145).

**Bug.** CJ has no CRASH_BIKE lines, so he never comments on crashes on a bike.

**Port.** CRASH_CAR and CRASH_BIKE fall back to CRASH_GENERIC when they find no sample, as
SilentPatch.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted.
