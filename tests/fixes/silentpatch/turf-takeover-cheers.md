# Turf takeover cheers

Group `turf-takeover-cheers` (byte_patch at 0x444059, nop at 0x444078, 12 call_redirects).

**Bug.** The district cheers are said by a gang member, and the district is matched against the
zone's info label, so CJ's cheers never play.

**Port.** As SilentPatch: the speaker is the player, and the twelve compares use the text label
(8 bytes in). The cheers' contexts are assigned in `speech-lookup-tables`.

**Reproduction.** Not scripted yet (needs a gang war won). The M9 route lists whether the compares
are reached.

**Result.** Lifted and converted.
