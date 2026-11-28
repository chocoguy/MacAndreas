# Speech lookup tables

Group `speech-lookup-tables` (mid_hook at WinMain, 0x748710).

**Bug.** Several speech contexts are unassigned or typo'd in the game's lookup tables. As a result,
CJ's turf takeover cheers, unused girlfriend and shopkeeper lines, gang taunts against the Triads,
Rifa and Da Nang Boys, and lines under misspelt contexts never play, and Maccer has no voice.

**Port.** The tables are patched once at WinMain, as SilentPatch does at load:
- The global table gets the player, girlfriend, generic and gang contexts.
- Typo'd contexts move to their correct slots: VBFYST2's generic contexts, VWMOTR1's MUGGED,
  ARREST_CRIM, SHOP_CLOSED, CAR_FIRE, JACKING_CAR_FEM and ATTACK_BY_PLAYER_LIKE. As in SilentPatch, a
  slot only moves when its destination is empty.
- The first free special voice slot becomes Maccer's.

The context numbers come from SilentPatch's `SpeechContextsSA.h`.

**Reproduction.** The M9 route checks that the tables and Maccer's slot are patched at load.

**Result.** See the route output. Hearing the restored lines needs the situations below.
