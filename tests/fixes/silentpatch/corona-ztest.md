# Coronas no longer force the Z test (PS2 sun)

Group `corona-ztest` (nop, 3 bytes at 0x6fb17c).

**Bug.** `CCoronas::Render` turns the Z test on for every corona, so the sun glare is hidden behind geometry,
unlike the PS2.

**Reproduction.** Not scripted yet: needs a pinned view of the sun behind an object, compared with and without.

**Result.** Lifted and converted; no reproduction yet.
