# Hydra jet thrusters

Group `jet-thrusters` (call_redirects at the four `CMatrix` multiplies in the Hydra's thruster
code). Contributed to SilentPatch by B1ack_Wh1te.

**Bug.** The product matrix is on the stack with uninitialised RwMatrix flags, so the thrusters
sometimes don't display.

**Port.** The product's flags are cleared after each multiply.

**Reproduction.** Not scripted yet (needs a Hydra).

**Result.** Lifted and converted.
