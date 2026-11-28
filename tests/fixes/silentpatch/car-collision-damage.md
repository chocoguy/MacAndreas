# Fixed car collisions

Group `car-collision-damage` (mid_hook at 0x5428ea).

**Bug.** Both cars in a collision take damage on the component chosen for the first one.

**Port.** The component byte is read from the side of the collision the impulse points to (+0x21 or +0x24).

**Reproduction.** Not scripted yet (needs two cars driven into each other).

**Result.** Lifted and converted; no reproduction yet.
