# Race condition in CdStream fixed

Group `cdstream-race` (mid_hooks in the DRM-moved `CdStreamSync`, 0x156cd9d, and in `CdStreamThread`, 0x406669).

**Bug.** `CdStreamSync` tests `nSectorsToRead` and then sets `bLocked`; the stream thread clears
`nSectorsToRead` and then tests `bLocked`. If the thread runs between the two, nobody releases the semaphore and
the game waits forever (black screen entering or leaving interiors). The port runs guest threads on real host
threads, so the race applies.

**Reproduction.** Not deterministic. Pending a stress run (many sync waits with `--serialize` off) counting waits.

**Port.** Both sides take one host mutex around the test-and-set; the wait itself happens unlocked.

**Result.** Lifted and converted; no reproduction yet.
