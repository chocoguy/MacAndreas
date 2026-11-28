# Car explosion crash with multimonitor (uninitialised collision data)

Group `collision-data-init` (call_redirects at the DRM-moved `CColModel` allocations: 0x1561793 `CMemoryMgr::Malloc`,
0x1564a1c and 0x156debd `operator new`).

**Bug.** New `CColData` is not cleared; stencil shadows read its flags and shadow mesh pointers and crash on car
explosions (more likely with more memory in use, as on multi-monitor setups).

**Reproduction.** The crash depends on stale heap contents and is not scripted. The route checks that new
collision data goes through the clearing allocator (`[SP] collision data`).

**Port.** As SilentPatch: flags, shadow triangle and vertex counts and pointers cleared after `Malloc`; flags
after `operator new`.

**Result.** The allocator is reached during loading (`[SP] collision data: new CColData flags and shadow mesh
cleared`) in every M9 route run. The crash itself is not reproduced.
