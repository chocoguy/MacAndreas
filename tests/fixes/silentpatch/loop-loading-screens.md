# Looping loading screens

Group `loop-loading-screens` (mid_hooks at 0x590ade and 0x590042).

**Bug.** A long load runs the splash index past the six loading screens, and the screen fades to white.

**Port.** As SilentPatch, `CLoadingScreen::DoPCScreenChange` wraps the index back to the first screen
after the last one. `RenderSplash` fades the splash that was shown before the wrap.

**Reproduction.** Not scripted (needs a load longer than six splashes). The M9 route lists whether the
loop happens.

**Result.** Lifted and converted; not reached (the route's loads fit in the splash list).
