# Mood after soliciting

Group `solicit-mood` (call_redirects at 0x666a82 and 0x666a9c).

**Bug.** CJ's mood is set to Wisecracking before his solicit line, so only Wisecracking lines play.

**Port.** The mood change is stored and applied after the line, as SilentPatch.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted.
