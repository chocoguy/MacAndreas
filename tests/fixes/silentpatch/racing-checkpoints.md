# Racing checkpoint colours

Group `racing-checkpoints` (call_redirect at 0x721520).

**Bug.** A racing checkpoint is drawn white when no other marker was drawn before it. Its geometry
doesn't modulate the material colour, and it depended on state left by another marker.

**Port.** As SilentPatch, the marker's geometries get `rpGEOMETRYMODULATEMATERIALCOLOR` before
`RpClumpRender`.

**Reproduction.** Not scripted yet (needs a race).

**Result.** Lifted and converted; not reached by the M9 route, which lists it.
