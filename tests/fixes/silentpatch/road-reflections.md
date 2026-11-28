# Wet road reflections (RefFix)

Group `road-reflections` (mid_hook at 0x6fb99e).

**Bug.** Wet road reflections don't render because their quad is placed at the engine's near-plane
depth.

**Port.** The quad's depth is 1.0, as SilentPatch's RefFix (Road Reflections Fix) sets.

**Reproduction.** Not scripted yet (needs rain at night and a capture). The M9 route lists whether the
hook is reached.

**Result.** Lifted and converted.
