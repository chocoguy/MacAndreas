# Moon phases

Group `moon-phases` (call_redirect at 0x713c4c).

**Bug.** The moon always shows full; the PS2 masks it by phase.

**Port.** As SilentPatch, `CClouds::Render`'s mask sprite is drawn through a moon mask into the alpha
channel only: the mask raster set as the texture, alpha test always, source alpha and zero blending,
and `D3DRS_COLORWRITEENABLE` limited to alpha. The game then draws the moon with destination alpha.

SilentPatch builds the mask texture with RenderWare's image internals and a code patch to
`RtPNGImageRead`, which the lifted build can't use. The port bundles SilentPatch's own `lunar64.png`
(`mods/silentpatch/lunar64.png`, MIT, by aap), compiles it into `sa-host`, decodes it with ImageIO and
fills a 64×64 8888 raster made with `RwRasterCreate` and `RwRasterLock`.

**Reproduction.** The moon is drawn from 0:00 to 7:20, brightest at 3:40, toward (0, -100, 15)
from the camera, and its phase follows the game's day count (31-day cycle). The M9 route sets the
clock to 3:40, advances it eight days and pins the camera at the moon (`--pin-camera`).

**Result.** Passed in all three M9 runs: `[SP] moon phases: 64x64 moon mask raster created` and `the
moon is masked by its phase`. In a capture eight days on, the masked moon is gibbous, cut off on the
left, where the stock moon (the original call alone) is full.
