# Fix a memory leak when taking photos

Group `photo-memory-leak` (mid_hooks at both returns of `psGrabScreen`, 0x7453ce and 0x7453d6).

**Bug.** `psGrabScreen` locks the back buffer surface and never unlocks or releases it, leaking a surface per photo.

**Reproduction.** `tools/route/m9_silentpatch.py` gives CJ the camera with
`--call …:0x5e6080:thiscall:*0xb7cd98,43,50,1` (`CPed::GiveWeapon`), selects it, aims and takes a photo. The hook
logs the reference count left after `Release`.

**Result.** Passed at 30 fps: after the photo, `psGrabScreen`'s surface is unlocked and released with 0
references left. Vanilla never releases it.
