# Fixed bicycle on fire

Group `bicycle-fire` (mid_hook at 0x53a9a7, call_redirects at 0x53a990 and 0x53a9b7).

**Bug.** Setting a BMX on fire sets CJ on fire, wherever he is.

**Port.** The fire goes to the bicycle's driver (+0x460), the player is not made to react, and no fire starts
without a driver.

**Reproduction.** Not scripted yet (needs a bicycle hit by a molotov or flamethrower).

**Result.** Lifted and converted; no reproduction yet.
