# Y axis sensitivity fix

Group `mouse-y-sensitivity` (byte_patch at 0x50f046, 0x50fb26, 0x510c26, 0x511e08, 0x52228c). By ThirteenAG.

**Bug.** The follow, aim and vehicle cameras multiply mouse Y by a separate vertical sensitivity (0xb6ec18),
which the menu slider never sets, so vertical look is slower than horizontal.

**Port.** Each `fmul [0xb6ec18]` becomes `fmul [0xb6ec1c]`, the horizontal sensitivity the slider sets.

**Reproduction.** Not scripted yet: needs the camera's pitch and yaw watched under equal mouse motions.

**Result.** Lifted and converted; no reproduction yet.
