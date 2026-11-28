# Cops' SURROUNDED and COVER ME lines

Group `cop-surrounded-lines` (call_redirects at four `GetPedsGroup` calls).

**Bug.** These lines need the speaker to be in a ped group, which cops never are.

**Port.** While more than one cop chases the player, cops get a dummy non-null group, as SilentPatch.
SilentPatch returns its module base as the dummy. The port returns a zeroed block the size of a
`CPedGroup`.

**Reproduction.** Not scripted yet. The M9 route lists whether the dummy is handed out.

**Result.** Lifted and converted.
