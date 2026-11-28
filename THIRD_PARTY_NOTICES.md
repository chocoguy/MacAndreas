# Third-Party Notices

MacAndreas is licensed under the GNU General Public License v3.0 or later (see `LICENSE`).
It incorporates or is built with the components below. Components are listed when they are
ported into or linked with MacAndreas, or bundled with a release. Tools used only on a
developer machine are listed separately.

MacAndreas contains no Grand Theft Auto: San Andreas code or assets. Users supply their own
`gta_sa.exe` 1.0 US, which is converted locally and never redistributed.

## Ported mod code

| Component | Upstream | License | Pinned commit |
|---|---|---|---|
| SilentPatch | https://github.com/CookiePLMonster/SilentPatch | MIT | `74976544adc196fa664831ddc59778e684cf6af0` |
| WidescreenFixesPack (GTA SA Widescreen Fix) | https://github.com/ThirteenAG/WidescreenFixesPack | GPL-3.0 | `d545cc29fc76b072590db0e6f91cb85932157eee` |
| Project2DFX (SA) | https://github.com/ThirteenAG/III.VC.SA.IV.Project2DFX | GPL-3.0 | `16509fcac44f0d0ea9eccc25d047055877c020b8` |

Pinned commits are the upstream revisions the inventory and port are based on (M7–M10).
SilentPatch's moon phase mask (`mods/silentpatch/lunar64.png`, by aap) is bundled unchanged and
compiled into `sa-host`.

## Linked or bundled libraries

| Component | Upstream | License | Used for |
|---|---|---|---|
| Zydis | https://github.com/zyantific/zydis (pinned v4.1.1, `a2278f1d254e492f6a6b39f6cb5d1f5d515659dc`) | MIT | x86 decoder for the fallback interpreter |
| Zycore | https://github.com/zyantific/zycore-c (Zydis submodule) | MIT | Support library required by Zydis |
| libvorbis / libogg | https://xiph.org/vorbis/ (libvorbis 1.3.7, libogg 1.3.6, statically linked from Homebrew for now) | BSD-3-Clause | Native replacement for `vorbisfile.dll` |
| pl_mpeg | https://github.com/phoboslab/pl_mpeg (vendored in `third_party/pl_mpeg`, pinned `c871f2be022ece7ef4f64230b4fb8e1fb9eb6023`) | MIT | Intro movie decoding |
| Remill | https://github.com/lifting-bits/remill (pinned `66c488c9911fd9b368e4d0c9647d74239f8bba8b`) | Apache-2.0 | x86 → LLVM IR lifting inside `sa-recomp` |
| LLVM (incl. lld) | https://llvm.org | Apache-2.0 WITH LLVM-exception | IR optimization, code generation and linking inside `sa-recomp` |

Remill semantics are compiled into the dylib each user generates locally. That output is
never distributed.

## Developer-only tools (not distributed)

| Component | Upstream | License | Used for |
|---|---|---|---|
| Ghidra | https://github.com/NationalSecurityAgency/ghidra | Apache-2.0 | Producing the committed address-only analysis data |

---

## License texts

### MIT License (SilentPatch)

Copyright (c) 2024 Adrian Zdanowicz (Silent)

### MIT License (Zydis, Zycore)

Copyright (c) 2014-2024 Florian Bernd
Copyright (c) 2014-2024 Joel Höner

### MIT License text (applies to each MIT component above)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### GPL-3.0 components

WidescreenFixesPack and Project2DFX are licensed under the GNU General Public License v3.0,
the same license as MacAndreas (see `LICENSE`). Their copyright notices are kept in the
headers of ported source files under `mods/widescreen` and `mods/project2dfx`.

### Apache-2.0 components

Remill, LLVM and Ghidra are licensed under the Apache License 2.0 (LLVM with the LLVM
exception). The full license text and each project's `NOTICE` file are included in release
bundles that ship `sa-recomp`.

### BSD-3-Clause components

libvorbis and libogg: Copyright (c) 2002-2020 Xiph.org Foundation. The full license text is
included in release bundles that link them.
