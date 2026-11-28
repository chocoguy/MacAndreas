# Fixed mirrors crash

Group `mirrors-crash` (byte_patch at 0x7271cb).

**Bug.** When beginning the mirror camera's update fails (the window is minimized next to a mirror), the mirror
render jumps past the code that puts `Scene.camera`'s raster and z-raster back, leaving the camera on the mirror
rasters; the next frame crashes.

**Reproduction.** Not scripted: the host's Metal layer does not fail `RwCameraBeginUpdate` on minimize. Pending a
forced-failure probe.

**Port.** SilentPatch's seven bytes: `test eax, eax / je 0x727203 / add esp, 4`, so the failure path restores the
rasters.

**Result.** Lifted and converted; no reproduction yet.
