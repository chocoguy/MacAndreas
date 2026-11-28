# Shadows on rotated objects

Group `shadow-rotated-objects` (mid_hook at 0x709821, nops at 0x7098c7 and 0x7098d2).

**Bug.** `CShadows::CastShadowEntityXY` ignores an object's up rotation, so shadows and lights cast
wrong on objects rotated along X.

**Port.** Each point is transformed by the entity's full matrix, as SilentPatch.

**Reproduction.** Not scripted yet (needs a capture of a light on a rotated object). The M9 route
lists whether the transform is reached.

**Result.** Lifted and converted.
