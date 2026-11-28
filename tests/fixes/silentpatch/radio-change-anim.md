# Radio station change animation

Group `radio-change-anim` (call_redirect at 0x6df4f4).

**Bug.** Changing the radio station plays an animation that moves CJ's whole upper body, which looks
wrong in many vehicles, including the Kart.

**Port.** As SilentPatch:
- No animation is played in the Kart (`ANIM_STD_CAR_SIT_KART`).
- Every bone except the head and right arm (clavicle, upper arm, forearm, hand, fingers, breast) loses
  its sequence.
- When driving a boat, the animation is no longer partial, so CJ doesn't reach as far.

**Reproduction.** Not scripted yet (needs CJ in a vehicle changing the station).

**Result.** Lifted and converted; not reached by the M9 route, which lists it.
