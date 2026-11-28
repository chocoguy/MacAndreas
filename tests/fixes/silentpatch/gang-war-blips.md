# Gang wars and friendly blips

Group `gang-war-blips` (mid_hook at 0x1560b5b).

**Bug.** When a gang war ends, `ReleasePedsInAttackWave` clears the blips of every ped flagged to
lose its blip on death, including CJ's recruits.

**Port.** The flag counts only for peds that are not in the player's group. The function is
DRM-moved (0x445c30 jumps to 0x15609c0). SilentPatch scans 0x200 bytes from the moved body. The
scanner took the first match in the image (0x5e452c), so the sites are pinned to 0x1560b5b in
`mods/silentpatch/pins.yaml`.

**Reproduction.** Not scripted yet (needs a gang war with recruits).

**Result.** Lifted and converted.
