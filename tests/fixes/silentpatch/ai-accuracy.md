# AI accuracy issue

Group `ai-accuracy` (mid_hook at 0x73b3ae).

**Bug.** The weapon range multiplier uses the ped's last vehicle even after it got out, so enemies who were in a car
earlier become too accurate.

**Port.** The vehicle is used only while the ped is in it (ped flags bit 0x100).

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted; no reproduction yet.
