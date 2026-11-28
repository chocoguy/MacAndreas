# Fixed CAEAudioUtility timers

Group `audio-utility-timers` (func_replace `sp_audio_utility_time_ms` at `CAEAudioUtility::GetCurrentTimeInMs`, 0x4d9e80).

**Bug.** The audio utility clock divides the performance counter as a float, which loses precision after days of
uptime; the dancing minigame's timing drifts. SilentPatch also fixes a division by zero there.

**Port.** Whole milliseconds since the host process started, from the monotonic clock, as a 64-bit integer.

**Reproduction.** `tools/route/m9_silentpatch.py` samples the function with `--call` at the two ends of the
600-frame window and compares the difference with the measured frame times.

**Result.** Passed: over 600 frames the function advances 19,998 ms against 19,998 ms of measured frame time.
