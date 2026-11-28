# Spawn lapdm1 correctly with PEDTYPE_COP

Group `lapdm1-biker-cop` (mid_hook at 0x464f68). By Wesser.

**Bug.** A script asking for lapdm1 as a cop gets a normal cop.

**Port.** SilentPatch repoints case 4 of the model switch; that jump table entry is shared with cases 0–2, so the
hook checks the case index (ecx) and returns cop type 1.

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted; no reproduction yet.
