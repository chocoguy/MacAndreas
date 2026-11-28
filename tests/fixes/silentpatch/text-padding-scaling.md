# Text box padding

Group `text-padding-scaling` (operand slots at 0x71a653, 0x71a66b, 0x71a69d, 0x71a6ab, 0x71a6bf and
0x71a6ec). Contributed to SilentPatch by Wesser.

**Bug.** The inner padding of text with a background (help boxes and similar) is a fixed number of
pixels.

**Port.** The horizontal padding is multiplied by the font width scale and the vertical padding by
the font height scale. SilentPatch recalculates at each `ProcessCurrentString`; the slots are
refreshed every frame and whenever the Widescreen menu canvas applies or restores.

**Reproduction.** Not scripted yet. The New Game help box uses this path; a capture comparison at two
resolutions is still to do.

**Result.** Lifted and converted.
