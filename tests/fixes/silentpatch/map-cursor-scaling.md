# Map screen cursor and boundaries

Group `map-cursor-scaling` (operand slots at 0x588251, 0x588265, 0x5882a8 and 0x5882c6,
call_redirect at 0x58822d). Contributed to SilentPatch by Wesser.

**Bug.** The cursor's lines are one pixel wide at any resolution, and `LimitToMap` compares unscaled
coordinates against scaled map edges, so the cursor's limits are wrong away from 640×448.

**Port.** The line sizes scale with the menu scales (read from `CMenuManager::StretchX`'s and
`StretchY`'s multipliers, which the Widescreen Fix narrows). The coordinates are scaled for the
`LimitToMap` call and unscaled after it. SilentPatch skips both parts when wshps.asi has changed the
code. The Widescreen port leaves these instructions alone, so both parts apply.

**Reproduction.** Not scripted yet (needs the Map screen with the cursor at its limits, at two
resolutions).

**Result.** Lifted and converted.
