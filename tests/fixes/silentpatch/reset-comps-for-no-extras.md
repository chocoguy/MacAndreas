# Reset requested extras if the vehicle has none

Groups `reset-comps-for-no-extras` (mid_hook at 0x4c97b1) and `extra6-any-rule` (byte_patch at 0x4c8010).

**Bugs.** A vehicle without extras leaves the requested components for the next one (lightless taxis, bikes
without engines); component rule 4 never picks extra 6.

**Port.** Used components are -1 and requested ones -2; rule 4 picks from six extras.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted; no reproduction yet.
