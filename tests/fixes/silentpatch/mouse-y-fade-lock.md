# Don't lock mouse Y axis during fade-ins

Group `mouse-y-fade-lock` (byte_patch at 0x50fbb4, 0x510512, 0x524071).

**Bug.** While the screen fades in, three camera modes ignore vertical mouse movement.

**Port.** The three fade checks are jumped over (to 0x50fbdd, 0x5105b3 and 0x524139), as SilentPatch.

**Reproduction.** Not scripted yet: needs a fade-in with vertical mouse motion and the camera pitch watched.

**Result.** Lifted and converted; no reproduction yet.
