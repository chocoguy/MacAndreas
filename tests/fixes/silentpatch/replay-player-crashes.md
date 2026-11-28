# Fix PlayerPed replay crashes

Group `replay-player-crashes` (call_redirects at 0x45f060, after `CReplay::RestoreStuffFromMem`, and at the
DRM-moved `DealWithNewPedPacket`'s `CClothes::RebuildPlayer` call, 0x156ee68).

**Bugs.** A mocap cutscene after a replay recorded in other clothes crashes; a replay recorded with another body
type crashes.

**Reproduction.** Not scripted yet (needs a recorded replay and a clothes or body change).

**Port.** Rebuild the player after restoring the game state; load the fat and muscular motion groups when the
replay rebuilds the player.

**Result.** Lifted and converted; no reproduction yet.
