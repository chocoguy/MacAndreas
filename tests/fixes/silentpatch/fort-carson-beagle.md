# Fort Carson Beagle

Group `fort-carson-beagle` (call_redirect at 0x406267).

**Bug.** A binary IPL car generator places a Beagle plane in Fort Carson.

**Port.** The generator with that exact position, rotation and model becomes a BMX, as SilentPatch.

**Reproduction.** The generator is in `countn2_stream1.ipl`, which streams in only near Fort Carson.
The M9 route finds its slot with `--call` to `CIplStore::FindIplSlot` (slot 6), requests it as
streaming resource 25255 + slot, and checks the replacement log.

**Result.** See the route output.
