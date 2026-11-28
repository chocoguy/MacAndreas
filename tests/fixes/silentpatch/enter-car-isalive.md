# Stealing a car from the passenger side

Group `enter-car-isalive` (byte_patch at 0x63f576: `jne` instead of `je`).

**Bug.** Holding throttle or brake while stealing a car from the passenger side kills the driver, or briefly
resurrects a dead one.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted; no reproduction yet.
