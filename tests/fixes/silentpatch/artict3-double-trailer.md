# Chained artict3 trailers

Group `artict3-double-trailer` (vtable_redirect at 0x871d18).

**Bug.** artict3 has a hook dummy that the game never uses as a tow bar.

**Port.** `CTrailer::GetTowBarPos` returns the hook dummy's position for model 591, or a point behind
its bounding box, as SilentPatch.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted.
