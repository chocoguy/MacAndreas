# Blurred helicopter rotors and plane propellers

Group `rotors` (call_redirect at 0x6c4523, mid_hook at 0x6cab80, imm_redirects at 0x7341d9,
0x734127, 0x73445e, 0x4c7b54 and 0x4c7b77, call_redirects at 0x73421d, 0x73414d and 0x73448c).
Setting `silentpatch.rotor_fix` (on).

**Bug.** The static and moving rotors are swapped at a fixed speed and drawn with alpha testing, so
the moving rotor never fades in as the blurred disc of Vice City and the PS2 version.

**Port.** As SilentPatch:
- Before `CHeli::Render` reaches `CEntity::Render`, and before `CPlane::Render`'s tail jump to
  `CVehicle::Render`, the static rotors fade out and the moving ones fade in with the rotor speed.
  Maverick (487) and Skimmer (460) keep their static rotors (SilentPatch's default exception list).
  With the setting off, every vehicle keeps its static rotor.
- The rotor and propeller alpha callbacks draw with vertex alpha on and alpha ref 0, whether they are
  drawn at once or from the sorted alpha list.
- When a helicopter's model is set up, `static_rotor` and `static_rotor2` get the rotor alpha callbacks.
  A plane's or big vehicle's `_prop` atomics get the big vehicle alpha callback.

**New manifest kind.** SilentPatch writes its callbacks into `push imm32` and `mov [esp+N], imm32`
operands. The port's `imm_redirect` points such an operand at a native at lift time. The host
registers the native at the site's redirect address and keeps the original pointer for
`redirect_original()` (ROADMAP §3.10).

**Reproduction.** The M9 route spawns a Hunter next to CJ (`CCheat::VehicleCheat`). It checks that the
static rotor gets its callback, the rotor alphas are set and the rotor is drawn with blending.

**Result.** Passed: the five `imm_redirect`s register with the expected originals (0x732b30, 0x4c7930, 0x4c7870); spawning the Hunter gives `static_rotor gets the rotor alpha callback`, `helicopter rotor alphas set` and `helicopter rotor drawn with blending`. At rest the Hunter shows its static rotor fully opaque. Plane propellers (`CPlane::Render`, `_prop` atomics) are converted but not reached.
