# Lights on vehicles and peds

Groups `lights-on-vehicles` (byte_patches at 0x5d9a87, 0x5d9a90 and 0x5d9f1e) and
`vehicle-extra-light` (byte_patches at six colour stores from 0x5d88d1, by aap).

**Bug.** Lights sometimes disappear from vehicles and peds: the car pipeline sets its own light in D3D
slot 1, over a light the scene set.
After SilentPatch's directional light fix, an extra light that compensated for it over-brightens
vehicles.

**Port.** As SilentPatch: the car pipeline's special light uses D3D light 8 instead of 1, so it no
longer overwrites a real light, and the extra light's colours are 0. The D3D9 layer now stores lights
above index 7 and lights with the first 8 enabled ones, as a driver does. Before that, the ignored
light 8 made CJ's lighting depend on earlier draws.

**Reproduction.** Not scripted yet (needs a capture of a car near street lights at night).

**Result.** Lifted and converted.
