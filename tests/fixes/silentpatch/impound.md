# Impound garages

Groups `impound-garages` (byte_patch at 0x425179, 0x425369, 0x425411), `impound-after-busted` (nop at 0x443292),
`impound-deleted-vehicles` (call_redirect at 0x6e2b6e) and `impound-vehicle-types` (call_redirects at 0x566794,
0x56a378).

**Bugs.** Impound garages never return the player's last car (the checks call the wrong garage function); being
busted does not impound the car; deleted traffic stays in the interesting vehicle list, so random cars get
impounded over the player's; helicopters, planes and boats can be impounded and then cannot leave.

**Reproduction.** Not scripted yet (needs a mission start or a bust with a car parked, and traffic deletions).
The route reports whether the deleted-vehicle and vehicle-type hooks are reached.

**Result.** The deleted-vehicle hook runs as traffic is deleted; the vehicle-type hook is not reached by the route. Bugs not reproduced yet.
