# Ped voice names

Group `speech-voice-names` (call_redirects at 0x5b75ab, 0x5b75bd and 0x5b749d).

**Bug.** peds.ide spells WMYSGRD's and BMYPIMP's voices differently from the audio tables, so those
peds are silent. SBFYSTR and SWFYSTR use the SBFYST and SWFYST voices.

**Port.** As SilentPatch, a failed voice lookup retries with the tables' spelling, and the two
models' voice names get their R suffix when peds.ide is parsed.

**Reproduction.** The M9 route checks the four fixes in the peds.ide load log.

**Result.** See the route output.
