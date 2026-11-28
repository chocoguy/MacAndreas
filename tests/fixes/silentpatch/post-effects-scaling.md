# Heat haze and underwater ripple scaling

Group `post-effects-scaling` (nop at 0x745c7d, mid_hook at 0x745c9f, call_redirect at 0x70529c).

**Bug.** `SetCurrentVideoMode` sets up the post-effect back buffer vertices before `RsGlobal` has the
new size, and the heat haze is not reinitialised, so both keep the old resolution after an in-game
change. The underwater ripple's frequency grows with the resolution.

**Port.** The early `SetupBackBufferVertex` call is removed. At the function's `ret`, the heat haze
type is reset to -1 and `SetupBackBufferVertex` runs with the new size. SilentPatch wraps the four
menu callers instead, but three of those calls are already Widescreen Fix call_redirects. Hooking
the callee covers every caller. The ripple's frequency argument is divided by height/480.

**Reproduction.** Partial: the M9 route lists whether the mode-change hook and the ripple are
reached. A scripted resolution change and a capture are still to do.

**Result.** See the route's reached list.
