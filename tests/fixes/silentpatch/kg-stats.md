# Stats in kilograms

Group `kg-stats` (data_poke at 0x863f04).

**Bug.** The stats pages pass a double to `%dkgs`, so weights display as garbage.

**Port.** The format string becomes `%.0fkg`, as SilentPatch. SilentPatch repoints the two pushes to
its own string. Only those two pages use the game's string, so the port rewrites the string itself.

**Reproduction.** Not scripted yet (needs the stats page with metric units).

**Result.** Lifted and converted.
