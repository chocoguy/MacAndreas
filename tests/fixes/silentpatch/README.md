# SilentPatch fix records

One file per ported, replaced or reworked SilentPatch fix group (ROADMAP M9). Each records the original bug,
how it is reproduced, what the port changes and the verification result. The group decisions themselves are in
`mods/silentpatch/triage.yaml`; manifest entries are in `manifest/silentpatch.yaml` (and `manifest/native.yaml`
for the native frame limiter).

Scripted checks:
- `tools/route/m7_silentpatch.py`: melee cheat ammo, units, jetpack hover, resolution string fallback.
- `tools/route/m9_silentpatch.py`: frame limiter and frame delay, timer fractions, zone bounds, script fixes,
  collision data, vehicles.ide defaults and the photo surface release, at 30, 60 and uncapped fps.

`sa-host --call FRAME:ADDR:CONV[:ARG,...]` calls a game function from the guest thread at a frame and logs the
result (`[CALL]`), for fixes that are easiest to reproduce by calling the patched function directly.
