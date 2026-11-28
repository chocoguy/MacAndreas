# Hierarchy typos

Group `hierarchy-typos` (call_redirect at 0x4c5311).

**Bug.** Typos in some vehicle hierarchies stop parts from being found: the DFT-30's middle wheel,
the Dumper's suspension and the Uranus tail lights.

**Port.** The node name compare also accepts SilentPatch's nine known typos.

**Reproduction.** The M9 route loads the DFT-30, Uranus and Dumper and checks that the DFT-30's
`wheel_lm` matches `wheel_lm_dummy`.

**Result.** See the route output.
