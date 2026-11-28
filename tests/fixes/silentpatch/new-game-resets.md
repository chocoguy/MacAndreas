# New Game resets

Group `new-game-resets` (call_redirects at 0x53c6db, 0x53c76d, 0x5b89e4, 0x5b89ee and 0x5b89f9).

**Bug.** Starting a New Game after loading a save keeps several variables from the save:
- free resprays and bombs;
- the ambulance and fire truck dispatch timers;
- seven stat counters (cycling, swimming under water, weapon, last weapon fired, deaths, max health,
  health added), so the player levels up faster;
- the "To stop Carl..." help text flag;
- the mad driver timer;
- the death and arrest penalties.

Pickups, car generators and stunt jumps from text IPLs are also lost.

**Port.** As SilentPatch, both of `CGame::InitialiseWhenRestarting`'s calls to
`ReInitGameObjectVariables`:
- reset those variables first, read through the same instruction operands SilentPatch reads;
- set the mad driver timer to 600–1200 s from the 31-bit generator, and the penalties to true;
- call the original;
- load the text IPL lines that the three loaders kept at startup again.

**Reproduction.** The M9 route starts a New Game from the menu and lists whether the reset ran.

**Result.** Lifted and converted; not reached. New Game from the boot menu does not go through `CGame::InitialiseWhenRestarting`, and calling it from a `--call` frame callback stops in the middle of the restart's data loading, so a scripted check needs New Game from the pause menu after a load.
