# Vehicle light beams

Group `lightbeam` (mid_hooks at the nine render state calls before the head light beam and the six
after it).

**Bug.** The head light beam changes render states and resets them to fixed defaults, leaving the
shade mode, culling and alpha test changed. As a result, vehicle lights get dark after being lit,
and windows vanish from inside when the lights are on.

**Port.** As SilentPatch: each state is saved before the beam and restored after it, including the
ones the game never reset. SilentPatch points the engine pointer operands at its own
`RenderStateSet`; the port handles the `call [reg+0x20]` instructions itself. SilentPatch's
`LightbeamFixExceptions` list is empty by default, so the per-vehicle exception is not ported.

**Reproduction.** Not scripted yet (needs a car with lights on at night). The M9 route lists whether the
restore runs.

**Result.** Lifted and converted.
