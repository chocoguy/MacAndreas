# Nitrous recharging while reversing

Group `nitrous-reverse` (mid_hook at 0x6a407b). Contributed to SilentPatch by Wesser.

**Bug.** Nitrous recharges at 1 - gas pedal; reversing makes the pedal negative, so it recharges
faster.

**Port.** A negative gas pedal is not subtracted, as SilentPatch's replacement code does.

**Reproduction.** Not scripted yet (needs a car with nitrous, reversing after use).

**Result.** Lifted and converted.
