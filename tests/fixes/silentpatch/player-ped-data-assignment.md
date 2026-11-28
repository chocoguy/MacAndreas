# Correct an improperly decrypted CPlayerPedData::operator=

Group `player-ped-data-assignment` (mid_hook at 0x1561d4d in the DRM-moved operator, 0x45c4b0 -> 0x1561cf0).

**Bug.** The HOODLUM decryption left nops where flag bits 0x1–0x100 are copied, so after a replay gang
recruiting stops working.

**Reproduction.** Not scripted yet (needs a replay, then a recruit attempt).

**Port.** The hook copies the bits exactly as SilentPatch's assembly, leaving edx and esi as the following code
expects.

**Result.** Lifted and converted; no reproduction yet.
