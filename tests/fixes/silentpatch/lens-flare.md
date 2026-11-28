# Fixed lens flare

Group `lens-flare` (byte_patch at 0x70f454, 0x6fb5ff and 0x6fb621; mid_hooks at 0x6fb480 and 0x6faf35).

**Bug.** The flare sprites are buffered across a Z test switch, so on drivers that honour the state change the
flare is never drawn (AMD and Intel on Windows).

**Port.** As SilentPatch: the buffer is flushed before the Z test changes and initialised after it; each flare
flushes at the end of its loop and the function returns without a second flush. The re-initialisation does
`CSprite::InitSpriteBuffer2D`'s three stores in the hook, because 0x70cfd0 is not a lifted trace head (calling it
went through the interpreter).

**Reproduction.** Not scripted yet: needs a pinned view into the sun.

**Result.** The flush hook runs when the sun is in view (camera pinned on the morning sun); the sun and glow render normally. The flare's visibility on its own is not compared yet.
