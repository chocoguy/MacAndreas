# Passenger lines when running over peds

Group `passenger-hit-ped-lines` (call_redirects at 0x5f05ca and 0x6a8298).

**Bug.** Passengers say `CAR_HIT_PED` when the car pushes a ped into a wall, but not when the player
runs a ped over.

**Port.** As SilentPatch:
- `CAutomobile::VehicleDamage` no longer says `CAR_HIT_PED` (the call returns -1, as if nothing was said).
- `CPed::KillPedWithCar`, for the player's vehicle, picks a random passenger, starting from a random seat.
  The passenger says `CAR_HIT_PED`, or `TAXI_HIT_PED` when they have no line for it.

The random seat comes from the host's 31-bit generator, as SilentPatch uses its own `rand()`, so the
game's random stream is untouched.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted; not reached on the route. Batch 11 corrected the passenger list offset
(0x464, after the driver at 0x460; batch 9 read from 0x468).
