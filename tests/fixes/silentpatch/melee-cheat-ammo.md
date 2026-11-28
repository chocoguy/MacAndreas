# Fixed ammo for melee weapons in cheats

Group `melee-cheat-ammo` (byte_patch), triage "Fixed ammo for melee weapons in cheats".

**Bug.** The weapon cheats give melee weapons, the chainsaw and the parachute with 0 ammo. A melee weapon with
no ammo is replaced by any melee weapon CJ walks over.

**Reproduction.** `tools/route/m7_silentpatch.py` types `PROFESSIONALSKIT` after New Game and watches the
player's melee slot (`[0xB7CD98]+0x5BC` weapon type, `+0x5C8` total ammo). Vanilla: knife with 0 ammo.

**Port.** Six whole-instruction rewrites (`push 1` instead of `push 0`; the katana's two pushes swapped).

**Result.** Passed (M7): the knife is given with total ammo 1.
