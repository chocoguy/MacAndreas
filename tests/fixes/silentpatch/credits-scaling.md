# Credits scaling

Group `credits-scaling` (mid_hooks at 0x5a8660 and 0x5af8c9, operand slots at 0x5a8689 and 0x5a869a,
call_redirects at 0x5a86c0, 0x5a8707 and 0x5a8785).

**Bug.** The credits are laid out in pixels, so at high resolutions they are small and scroll too
slowly. They also stop 20 units above the bottom edge.

**Port.** As SilentPatch:
- `CCredits::PrintCreditText` and the end check in `CCredits::Render` use a fixed 448-unit height.
- `CFont::SetScale` and the two `CFont::PrintString` calls scale x and y by the 2D scales (the
  multipliers read from the game's `Render2dStuff` instructions, so a Widescreen Fix narrowing
  follows). The second print's offset of 1 becomes one scaled unit.
- The margins become 1 at the top and -20 at the bottom.

**Reproduction.** The M9 route starts the credits with `--call 0x7170e0` (`CCredits::Start`) after
the photo and captures a frame 40 frames later.

**Result.** The route checks that the credits go through the fixed-height placement and the scaled
print. The captured frame is not compared automatically.
