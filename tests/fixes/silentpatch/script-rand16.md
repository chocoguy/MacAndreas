# Script randomness is 16-bit

Group `script-rand16` (call_redirects at 0x4674fe and 0x467533).

**Port.** A second rand() supplies bit 15, as SilentPatch.

**Result.** Not reached by the New Game route (no script asks for a random number in that stretch); no reproduction yet.
