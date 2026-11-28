# Jumping on a bike from the front

Group `bike-front-jump` (byte_patch at 0x64ffad). Contributed to SilentPatch by B1ack_Wh1te.

**Bug.** CJ sinks into the bike when jumping on from the front: the z offset is added.

**Port.** `fadd` becomes `fsub`, as SilentPatch.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted.
