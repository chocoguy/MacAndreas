# Wheels lost by exploding cars

Group `exploding-car-wheels` (call_redirects at 0x6b38ca, 0x6b3ccb, 0x6c6ebe and 0x6ccef9, nops at
0x6b38e4, 0x6b3cf1, 0x6c6ed8 and 0x6ccf17, operand_slot at 0x6c25f5).

**Bug.** An exploding car loses a random wheel in its damage manager, but the front left wheel always
flies off. The random pick never chooses the rear right wheel.

**Port.** As SilentPatch:
- The `BlowUpCar` paths spawn and hide the wheels whose status is detached: front left, front right,
  and the rear wheels with their middle wheels. They no longer hide the front left wheel unconditionally.
- `CDamageManager::FuckCarCompletely` picks from rand × -4 instead of -3, through an operand slot.

**Reproduction.** Not scripted yet (needs a car blown up).

**Result.** Lifted and converted; not reached by the M9 route, which lists it.
