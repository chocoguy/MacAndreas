# Shooting stars render white

Group `shooting-stars` (call_redirect at 0x714610).

**Bug.** A texture left bound turns shooting stars black.

**Port.** The texture raster is unset before the stars' RwIm3DTransform.

**Reproduction.** Not scripted yet (needs a night sky with a shooting star).

**Result.** Lifted and converted; no reproduction yet.
