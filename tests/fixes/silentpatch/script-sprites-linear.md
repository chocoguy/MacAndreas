# Linear filtering on script sprites

Group `script-sprites-linear` (call_redirect at 0x58c092, `CTheScripts::DrawScriptSpritesAndRectangles`).

**Bug.** Script sprites (minigame and mission UI) are drawn with point filtering.

**Port.** The texture filter is set to linear around the call and restored.

**Reproduction.** Not scripted yet: needs a minigame screen captured with and without.

**Result.** Lifted and converted; no reproduction yet.
