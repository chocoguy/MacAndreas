# Good Citizen Bonus

Group `good-citizen` (mid_hook at 0x532092, call_redirect at 0x5320c7).

**Bug.** The bonus checks a legacy flag that is never set, so it never triggers.

**Port.** As SilentPatch:
- A ped killed while fleeing from a cop (EVENT_PED_TO_FLEE) gives the bonus once; the flag is then
  used to remember it.
- The cop says GOOD_CITIZEN.
- A dying or already counted ped is skipped.
- GOODBOY is missing from the GXT, so the text falls back to "$50 Good Citizen Bonus!".

SilentPatch's two injected calls become one mid_hook that resumes at the bonus, the skip or the
punishment check.

**Reproduction.** Not scripted yet (needs a ped fleeing from a cop). The M9 route lists whether a
bonus is given.

**Result.** Lifted and converted.
