# Stencil shadow memory

Group `stencil-shadow-pads` (mid_hook at 0x711dd5, nop at 0x711e0d, byte_patch at 0x711e5f).

**Bug.** Three 24 KB buffers are allocated and freed every frame for stencil shadows.

**Port.** They are allocated once from guest memory and kept.

**Result.** Reached: the pads are allocated once at the first stencil shadow.
