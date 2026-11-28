# Number plates

Groups `number-plates` (call_redirects at 0x4c9660, 0x6d6a58, 0x6d651c and 0x6d0e53, nops at 0x6d6517
and 0x6d0e43, func_replace at 0x6fdfe0) and `plate-filtering` (byte_patch at 0x6fdf44).

**Bug.** Cars of the same model share one plate. Custom plates don't always show and are never
allowed on bikes. Plates are point filtered.

**Port.** As SilentPatch:
- Each vehicle creates its own plate texture, from the model's custom text or a generated one.
- The plate and plate-back materials are swapped in before each render and restored after, for bikes
  too.
- Plate textures use bilinear filtering.

**Reproduction.** Partial: the M9 route lists whether vehicles get their own plates.

**Result.** See the route's reached list.
