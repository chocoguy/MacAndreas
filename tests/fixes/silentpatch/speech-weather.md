# Weather conversations

Group `speech-weather` (byte_patch at 0x43b341, call_redirects at 0x43be4a, 0x4e67e2 and 0x4e5a67).

**Bug.** Peds never pick the weather topic, and CJ's replies to it have no samples in his current
mood.

**Port.** As SilentPatch:
- The topic pick includes AE_CONV_WEATHER.
- A positive reply to a negative weather comment uses WEATHER_DISL_REPLY.
- CJ's weather replies retry with the CD mood when no sample is found.

**Reproduction.** Not scripted yet (needs a ped conversation). The M9 route lists whether the reply
hook is reached.

**Result.** Lifted and converted.
