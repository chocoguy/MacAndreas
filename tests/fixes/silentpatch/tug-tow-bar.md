# Tug tow bar

Group `tug-tow-bar` (mid_hook at 0x6af2cc).

**Bug.** The Tug's tow bar dummy is `misc_b`, but the game only looks for `misc_a`.

**Port.** The tow bar frame falls back to `misc_b`, as SilentPatch.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted.
