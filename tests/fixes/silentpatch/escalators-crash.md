# Fixed escalators crash

Group `escalators-crash` (call_redirect `sp_escalator_switch_off` at 0x71791f, mid_hook `sp_escalators_update` at
0x7185b5).

**Bug.** `CEscalator::SwitchOff` deletes the step objects while the escalators are being processed, which crashes
occasionally near escalators.

**Reproduction.** Not scripted yet (needs a route to an escalator and the switch-off distance).

**Port.** Switching off queues the steps; before the next `CEscalators::Update` they are removed from the world
and deleted.

**Result.** Lifted and converted; no reproduction yet.
