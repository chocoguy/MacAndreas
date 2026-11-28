# Mission title and 'Mission Passed' durations

Group `sliding-text-thresholds` (byte_patch at 0x58d2e9, mid_hook at 0x58c90e).

**Bug.** The big messages count a cut sliding-text position up to the screen width minus 20. The
step is in 640-unit space, so at high resolutions the texts stay on screen longer.

**Port.** Both thresholds use 640 (620 after the margin) instead of `RsGlobal.maximumWidth`, as
SilentPatch. SilentPatch's optional `SlidingMissionTitleText` and `SlidingOddJobText`, which
re-enable the slide, are ported as settings (`sliding-texts.md`).

**Reproduction.** Not scripted yet (needs a mission title or 'Mission Passed' message, timed at two
resolutions).

**Result.** Lifted and converted. The mid_hook logs when it is reached; New Game reaches it only if a
big message shows.
