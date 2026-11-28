# Weapons seen through vehicle windows

Group `weapon-rendering` (call_redirect at 0x5e7859, func_replace of 0x732f30).

**Bug.** `CPed::Render` only puts the ped on a list; `RenderWeaponPedsForPC` draws every listed weapon
after the vehicles' alpha pass, so a weapon held inside a car is hidden behind its windows. It also
sets up each ped's lighting and never removes it, which lights some objects drawn later incorrectly.

**Port.** As SilentPatch:
- `CPed::Render` draws the weapon right after the ped (a native `CPed::RenderWeapon`: the hand bone's
  matrix, the parachute offset, the second gun of twin-pistol weapons), then lists the ped as before.
- `RenderWeaponPedsForPC` draws only the muzzle flashes, with Z writes off, between each ped's
  `SetupLighting` and `RemoveLighting`. It sets vertex alpha, Z writes and fog and restores them.

The native `RenderWeapon` is shared with the shadow fix (`shadow-camera-update.md`).

**Reproduction.** The M9 route gives CJ the camera and checks that the weapon is drawn with the ped.
A weapon in a car is not scripted yet.

**Result.** Passed: with the camera in CJ's hand, `[SP] weapons: rendered with the ped, muzzle flash deferred`; the camera shows in his hand in a capture after the Hunter spawns. M4, M5, M7 and M8 pass.
