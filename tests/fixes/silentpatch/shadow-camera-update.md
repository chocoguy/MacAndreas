# Weapon and jetpack shadows

Groups `shadow-camera-update` (byte_patch at 0x70665c, call_redirect at 0x706662).

**Bug.** A real-time shadow is drawn from the entity's clump alone, so a ped's weapon and jetpack cast
no shadow.

**Port.** As SilentPatch, `CRealTimeShadow::Update` passes the entity (`push edx`) instead of its clump,
and a native `CShadowCamera::Update`:
- clears the shadow raster;
- draws a ped's clump, its weapon (through the native `RenderWeapon` of `weapon-rendering.md`) and
  its jetpack;
- draws any other entity's atomics;
- inverts the raster.

Atomics that are already instanced render with their texture, lighting and colour flags stripped and
then restored, as SilentPatch's `ShadowCameraRenderCB` does.

**Reproduction.** The M9 route checks that the shadow camera update runs with CJ holding the camera.

**Result.** Passed: `[SP] shadows: CShadowCamera::Update takes the entity` and `[SP] shadows: a ped's weapon is drawn into its shadow` with the camera. Jetpack shadows are not scripted yet.
