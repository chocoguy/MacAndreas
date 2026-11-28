# Vehicle light switching randomness

Group `vehicle-lights-randomness` (operand_slot at 0x6d5612).

**Bug.** The PS2's threshold of 50000 out of 65535 can't be reached with the PC's 15-bit rand.

**Port.** The scale becomes 1/25000.

**Reproduction.** Not scripted yet (needs traffic counted at dusk).

**Result.** Lifted and converted; no reproduction yet.
