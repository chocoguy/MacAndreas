# Delay destroying of cigarettes/bottles held by NPCs

Group `delayed-prop-destruction` (byte_patch at 0x5e03d4).

**Bug.** Recruiting homies who hold a cigarette or bottle destroys the prop during `CPlayerPed::ProcessControl`,
while `CWorld::Process` holds a cached pointer to the next node of the moving list; mashing replay near such
gangs crashes.

**Reproduction.** Not scripted yet (needs a gang group holding props next to CJ).

**Port.** `or [esi+1Ch], 800h` (bRemoveFromWorld) and `jmp 0x5e03ec`, so the world removes the prop later; four
nops pad to the next instruction.

**Result.** Lifted and converted; no reproduction yet.
