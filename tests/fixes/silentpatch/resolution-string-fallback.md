# Display a fallback string if the resolution string is absent

Group `resolution-string-fallback` (call_redirect `sp_ascii_to_gxt_null_check` at 0x57a071).

**Bug.** On 1.0 with little VRAM, Advanced Display Options passes a null mode string to `AsciiToGxtChar` and
crashes (fixed in 1.01).

**Reproduction.** The native mode list never produces a null string, so the route checks that the call goes
through the redirect and back: `tools/route/m7_silentpatch.py` opens Advanced Display Options.

**Result.** Passed (M7): the redirect is reached with `"1280 X 720 X 32"` and the row draws unchanged. The null
substitution itself is covered only by code review.
