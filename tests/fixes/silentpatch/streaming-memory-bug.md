# "Streaming memory bug" fix

Group `streaming-memory-bug` (call_redirect at 0x4c51a9, the call to `RtAnimInterpolatorSetCurrentAnim`).

**Bug.** With high streaming memory, ped animations show artifacts: RenderWare's version writes key frame pointers
into each interpolated frame, past what GTA's smaller interpolated frames hold.

**Port.** SilentPatch's `GTARtAnimInterpolatorSetCurrentAnim`: the interpolator is set up and the first two key
frames interpolated, without the pointer loop. The structure offsets were checked against the game's own function
(0x7cd5a0).

**Reproduction.** The artifact needs high streaming memory use; the route checks that animations go through the
replacement.

**Result.** Passed: animations start through the replacement in the New Game route; the artifact itself is not reproduced.
