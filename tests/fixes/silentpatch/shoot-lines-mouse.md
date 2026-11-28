# SHOOT lines with mouse and keyboard

Group `shoot-lines-mouse` (mid_hook at 0x7427ef, nop at 0x7427fc, call_redirect at 0x74281a).

**Bug.** CJ's shooting lines only play with gamepad auto-aim.

**Port.** As SilentPatch: the fired weapon's target is stored, and the line plays when it, or the
mouse lock-on recruit ped, is a ped.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted.
