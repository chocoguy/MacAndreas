# Menu and replay line wraps

Group `line-wrap-scaling` (call_redirects in `CMenuManager::PrintMap`, `DrawStandardMenus` and
`CReplay::Display`).

**Bug.** Several wrap widths are fixed pixel distances, so menu texts wrap differently at each
resolution.

**Port.** As SilentPatch: the full-width wrap is the screen width, left-measured wraps are scaled by
the menu width scale, and right-measured wraps scale their distance from the right edge (by the
replay text's scale in `CReplay::Display`).

**Reproduction.** Partial: the M9 route lists whether the menu wraps are reached. A comparison at
two resolutions is still to do.

**Result.** See the route's reached list.
