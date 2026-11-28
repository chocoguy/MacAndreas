# Correct streaming when using RC vehicles

Group `rc-streaming` (call_redirects at 0x55574b and 0x5557c3 in the streaming update).

**Bug.** Streaming centres on CJ while he drives an RC vehicle, so the area around the RC car shows LODs (Supply
Lines).

**Port.** The focus is the RC vehicle (CPlayerInfo +0xb0), then the player's vehicle, then the player; the speed
check includes the RC vehicle.

**Reproduction.** Not scripted yet (needs an RC vehicle).

**Result.** Lifted and converted; no reproduction yet.
