# Heat-seeking and gamepad crosshair scaling

Group `crosshair-scaling` (call_redirects at six `RenderOneXLUSprite_Rotate_Aspect` calls,
operand slots on 13 triangle sizes).

**Bug.** The heat-seeking missile crosshair, the co-op in-car crosshair and the triangular gamepad
crosshairs are sized in pixels, so they shrink at high resolutions.

**Port.** As SilentPatch: the sprite width and height are multiplied by the 2D width and height
scales, and the triangle sizes by the 2D height scale (slots refreshed every frame and with the menu
canvas). With the Widescreen Fix, its lock-on widths (20 instead of 28) feed in first, as on Windows.

**Reproduction.** Not scripted yet (needs a heat-seeking lock-on or a gamepad).

**Result.** Lifted and converted.
