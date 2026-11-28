# Proper metric-imperial conversion constants

Group `metric-imperial-constants` (operand_slot `sp_meters_to_feet` at 0x55942f and 0x55aa96).

**Bug.** The stats pages convert metres to feet with a rounded constant.

**Port.** Both multiplies read 3.280839895 from their own operand slot.

**Reproduction.** Not scripted yet: needs the stats page with imperial units and a known distance stat.

**Result.** Lifted and converted; no reproduction yet.
