# Map screen seams

Group `map-screen-seams` (six byte_patches in `CMenuManager::PrintMap`).

**Bug.** With anti-aliasing, single-pixel seams show between the Map screen's tiles.

**Port.** The tile rectangles start at -0.5 instead of 0, as SilentPatch.

**Reproduction.** Not scripted yet (needs a Map screen capture with MSAA).

**Result.** Lifted and converted.
