# Static and stored shadows: alpha test

Groups `static-shadow-alpha` (call_redirect at 0x53e0c3) and `stored-shadow-alpha` (call_redirect at 0x53e0c8).

**Bug.** Shadows from fire and under pickups are cut by the alpha test left on by earlier rendering.

**Port.** Both shadow passes set the alpha test function to always and restore it afterwards.

**Reproduction.** Not scripted yet: needs a pickup or fire shadow captured.

**Result.** Reached in the New Game route; the shadows themselves are not compared yet.
