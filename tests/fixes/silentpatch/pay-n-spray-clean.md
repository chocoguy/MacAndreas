# Pay 'n Spray cleaning

Group `pay-n-spray-clean` (nop at 0x44acdc).

**Bug.** Pay 'n Spray cleans the car before the doors close, while it is still in view.

**Port.** As SilentPatch, the early dirt reset is skipped; the car is cleaned behind the doors.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted.
