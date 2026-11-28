# Mirrors depth fix and bumped quality

Group `mirror-buffers` (call_redirect at 0x72701d).

**Bug.** With anti-aliasing the mirror render shares a mismatched depth buffer and breaks; its resolution is low.

**Port.** The mirror raster and its own z-raster are created without MSAA, 2048×1024 at Very High visual FX
quality, 1024×512 at Medium and High, 512×256 at Low.

**Reproduction.** Not scripted yet (needs a room with a mirror); the route reports whether the buffers are created.

**Result.** Not reached by the route; no reproduction yet.
