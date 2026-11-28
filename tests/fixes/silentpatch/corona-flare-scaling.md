# Flare effects scale to resolution

Group `corona-flare-scaling` (call_redirects at 0x6fb461, 0x6fb561, 0x6fb5ea).

**Bug.** Flare sprites have fixed pixel sizes, so they shrink at high resolutions.

**Port.** Width and height are multiplied by RsGlobal's size times the 2D scale constants (SilentPatch's
`UIScales::Stuff2d`); the Widescreen Fix's narrowed 1/640 applies, as it does with both mods on Windows.

**Result.** Reached: at 1280×720 with the Widescreen Fix on, flare sprites scale by 1.5 × 1.5.
