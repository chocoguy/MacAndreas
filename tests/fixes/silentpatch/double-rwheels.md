# Dual rear wheels

Group `double-rwheels` (byte_patch at 0x4c9290, mid_hooks at 0x4c9223 and 0x4c92f4).

**Bug.** The second rear wheel of `DOUBLE_RWHEELS` vehicles (Yosemite, Tanker and others) copies the
wheel frame with its matrix flagged as identity, so it sits inside the first wheel. Trailers only get
the second wheel on the rear wheels.

**Port.** As SilentPatch:
- The copied matrix's type and identity flags are masked off (`and edx, ~0x20003`), not set.
- Trailers get dual wheels on every wheel. The mirrored-wheel path also reaches this check; the
  mid_hook restores the stack and wheel id as SilentPatch's stub does.

SilentPatch's `[DoubleRearWheels]` INI list is dropped (empty by default; see triage).

**Reproduction.** Not scripted yet. The M9 route lists whether a trailer reaches the check.

**Result.** Lifted and converted; no trailer is loaded on the route.
