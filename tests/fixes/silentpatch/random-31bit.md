# Proper randomizations

Group `random-31bit` (call_redirects at 0x44e82e, 0x44ecee, 0x666ea0).

**Bug.** Ped path selection and prostitute solicitation were written for the PS2's 31-bit random; with the PC's
15-bit rand some ped paths never spawn and prostitutes stay quiet.

**Port.** The three calls use the PS2 generator (`seed = 0x5851F42D4C957F2D * seed + 1`, bits 32–62).

**Reproduction.** Not scripted yet.

**Result.** Lifted and converted; no reproduction yet.
