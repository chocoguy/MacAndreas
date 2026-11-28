# 014C car generator counter fix

Group `cargen-counter` (byte_patch at 0x6f3e2c and 0x6f3e32). By spaceeinstein.

**Bug.** Generators with a fixed number of spawns read the counter unsigned, so the count never runs out.

**Reproduction.** Not scripted yet: needs a script generator with a count.

**Result.** Lifted and converted; no reproduction yet.
