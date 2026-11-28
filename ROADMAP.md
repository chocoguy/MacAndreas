# MacAndreas Roadmap

A native Apple Silicon port of GTA: San Andreas (PC 1.0 US) made by **static recompilation**
of the user's own `gta_sa.exe`. SilentPatch, the Widescreen Fix and Project2DFX are baked in.

This document expands the original `PLAN.md`. It covers the decisions made so far, the
architecture in enough detail to start building, and milestones M0–M12 with work items,
deliverables, exit criteria and dependencies. There are no time estimates; milestones are
ordered by dependency.

---

## 0. Decisions

| Topic | Decision | Notes |
|---|---|---|
| Project license | **GPL-3.0-or-later** | Required because WidescreenFixesPack and Project2DFX are GPL-3.0 (the original plan listed Project2DFX as MIT; its repo is GPL-3.0). |
| Fallback interpreter | **In-house x86-32 interpreter** (no Unicorn) | Unicorn is GPL-2.0-only through QEMU, so it can't ship in one binary with GPL-3.0 code. Our interpreter shares guest state with lifted code. |
| Who converts | **Hybrid** | Address-only analysis data is committed. The launcher bundles a pinned Remill + LLVM converter and converts locally. Users never need Ghidra or Java. |
| Accepted input | `gta_sa.exe` 1.0 US, 14,383,616 bytes, SHA-256 `a559aa772fd136379155efa71f00c47aad34bbfeae6196b0fe1047d0645cbd26` | This is the standard 1.0 US build with a `.HOODLUM` section, i.e. what downgraders produce. All three mods target it. |
| Host platform | **Apple Silicon only, macOS 26+** | Metal 4 era APIs, current GameController.framework, BC texture support. No Intel or Rosetta builds. |
| Threads | Real host threads | A debug "serialize" mode runs all guest threads under one global lock, to tell races apart from logic bugs. |
| SilentPatch FLAC | Dropped | Vorbis streams through native libvorbisfile only. |
| DXVK bootstrap | Dropped | The fake D3D9 layer (§3.6) reaches first frame without a Vulkan stack. |
| Graphics strategy | **D3D9 subset on Metal first, native RW driver later** | Changed from the original plan; see §3.6 for why. |
| Time estimates | None | Milestones carry exit criteria and dependencies only. |

### Changes from the original plan

1. GPL-3.0-or-later plus an own interpreter replaces "Unicorn + undecided license".
2. The hook manifest **engine** moves to M1, because runtime overrides (`func_replace`) are
   needed from first boot. M7 becomes the mod-facing tooling: inventory, pinning, triage and
   strict conflict checks.
3. Graphics order is reversed. Unmodified lifted RenderWare talks to a native D3D9-subset
   device first; native RW driver overrides come later for hot paths only.
4. New M12 (release). Launcher work is spread across M0 (verify), M6 (MVP) and M12 (polish).
5. The project is named **MacAndreas**. User files go to `~/Library/Application Support/MacAndreas/`.

---

## 1. Ground Rules

- **Never commit game files or translated game code.** That means no exe, no lifted IR, no
  `.o`/`.dylib` built from game code, no game assets, no shaders extracted or translated from
  the exe, and no code bytes copied from the exe.
- **Committed analysis data is allowed only if it is address-only.** Function starts and ends,
  jump-table bounds, call targets, calling conventions and arities are fine. Instruction bytes
  and disassembly text are not. A CI check enforces this (M0).
- **Only one input is supported**, the hash above. Anything else is rejected. For known
  Steam/RGL hashes, the message tells the user to downgrade to 1.0 US.
- **Guest memory uses the original x86 addresses** inside a 4 GB guest region:
  `host = guest_base + guest_addr`. The image is never relocated. Literal host addresses can't
  be used because arm64 macOS processes reserve the low 4 GB as `__PAGEZERO`.
- **Mod code is ported, not loaded.** There is no ASI loader and no guest DLL loading. Guest
  DLL imports (`vorbisfile.dll`, `EAX.DLL`) get native implementations.
- **Keep upstream license notices** in `THIRD_PARTY_NOTICES.md`. Check every dependency's
  license before linking it.
- **Code style:** no comments in code unless explicitly requested.

---

## 2. Input Facts (`gta_sa.exe` 1.0 US)

Image base `0x00400000`, entry point `0x00824570`, no PE TLS directory. There are 11 sections,
several of them SecuROM leftovers:

| Section | VA | VSize | Flags | Notes |
|---|---|---|---|---|
| `.text` | `0x00401000` | `0x456000` | RX | Main game + RenderWare code |
| `_rwcseg` | `0x00857000` | `0x1000` | RX | RW code segment |
| `.rdata` | `0x00858000` | `0x4c000` | R | |
| `.data` | `0x008a4000` | `0x3fa000` | RW | 256 KB raw, the rest is BSS |
| `_TEXT_HA` | `0x00c9e000` | `0x11000` | RW | |
| `_rwdseg` | `0x00caf000` | `0x1000` | RW | |
| `.rsrc` | `0x00cb0000` | `0x1000` | R | |
| `.text` (2) | `0x00cb1000` | `0x64a000` | **RWX** | Protection-wrapped code; expect obfuscated control flow |
| `.init` | `0x012fb000` | `0x6000` | RWX | |
| `.data` (2) | `0x01301000` | `0x255000` | RWX | |
| `.HOODLUM` | `0x01556000` | `0x21000` | RWX | Crack-relocated code; some game functions live here |

**Import surface: 214 imports across 9 DLLs.**

| DLL | Count | Strategy |
|---|---|---|
| KERNEL32 | 138 | Native Win32 runtime: files, threads, sync, heap, virtual memory, locale, CRT support, SEH |
| USER32 | 44 | Window, message loop, cursor and input over AppKit. `DialogBoxParamA` returns a fixed result. |
| ADVAPI32 | 6 | Virtual registry backed by a settings file |
| WS2_32 | 10 (ordinals) | Stub with failure codes (leftover network code) |
| WINMM | 4 | `timeGetTime` etc. over `mach_absolute_time` |
| ole32 | 3 | COM init plus `CoCreateInstance` (likely DirectShow for movies; confirm CLSIDs) |
| vorbisfile.dll | 7 | Native libvorbisfile (BSD) with guest I/O callbacks |
| EAX.DLL | 1 (ordinal 6) | Native fake DirectSound8 + EAX on CoreAudio |
| GDI32 | 1 | `DeleteObject` no-op |

**D3D9 and DInput8 are not in the import table.** They are loaded through
`LoadLibraryA`/`GetProcAddress`, so the runtime serves a native module table
(`d3d9.dll` → `Direct3DCreate9`, `dinput8.dll` → `DirectInput8Create`). That lets the whole RW
D3D9 driver run as lifted code against our device.

---

## 3. Architecture

```
┌───────────────────────────────────────────────────────────────────┐
│ Launcher (Swift): verify exe, convert (bundled sa-recomp),        │
│   settings, launch                                                │
├───────────────────────────────────────────────────────────────────┤
│ sa-host (native executable)                                       │
│  ├ libgta_sa_native.dylib   lifted game code (generated locally)  │
│  ├ Function table           guest addr → host fn (two-level)      │
│  ├ Interpreter              x86-32 fallback, shares GuestCtx      │
│  ├ Manifest engine          patches, redirects, hooks, replaces   │
│  ├ Guest memory             4 GB region, original addresses       │
│  └ Native thunk range       imports, COM vtables, callbacks       │
├──────────────┬──────────────────┬──────────────┬──────────────────┤
│ Win32 runtime│ Graphics         │ Audio        │ Input            │
│ kernel32,    │ D3D9 subset →    │ DSound8/EAX →│ DInput8 + WM_*   │
│ user32, reg, │ Metal; later     │ CoreAudio;   │ → AppKit events, │
│ winmm, ws2   │ native RW paths  │ libvorbis    │ GameController   │
├──────────────┴──────────────────┴──────────────┴──────────────────┤
│ VFS: case-insensitive game dir + mod data overlay + user dir      │
├───────────────────────────────────────────────────────────────────┤
│ Mods (native C++): silentpatch / widescreen / project2dfx         │
└───────────────────────────────────────────────────────────────────┘
```

### 3.1 Guest address space

- At startup, reserve 4 GB (+ guard) at a 4 GB-aligned `guest_base`. Every guest access is
  `guest_base + zext32(addr)`, so a bad pointer can't escape the region.
- Layout:
  - `0x00000000–0x0000FFFF` no access (null guard).
  - `0x00400000–0x01577000` the image, mapped section by section with the original protections.
  - Guest stacks, TEB/PEB blocks, heap arenas, `VirtualAlloc` and file-mapping regions come
    from a guest-VA allocator.
  - `0xF0000000+` is the native thunk range. Each thunk address is registered in the function
    table and points to a native implementation.
- `byte_patch` entries are applied to the exe bytes before lifting **and** to the mapped image,
  so data reads, the interpreter and lifted code all see the same bytes.
- In debug builds, lifted code ranges are write-protected. Any write raises a
  self-modifying-code report (the RWX sections make this worth checking).

### 3.2 Execution model

- **GuestCtx** is per thread: GPRs, EFLAGS, x87 stack/control/status/tag, SSE, segment bases
  (FS → that thread's TEB) and the current guest PC. Remill's `State` struct is the GuestCtx.
- **Lifted functions** share one host signature. Guest `call` pushes the real guest return
  address onto the guest stack and makes a host call. Guest `ret` pops it and returns natively.
  Debug builds check that the popped address matches the call site.
- **Indirect calls and jumps** go through the function table (two-level, per 4 KB page,
  allocated lazily). If an address is missing, the interpreter runs it and logs
  `[INTERP] 0xADDR caller=0xADDR` once per address.
- **Interpreter**: Zydis (MIT) decoder plus our own semantics for the integer, x87 and SSE
  subset the game uses. It reads and writes the same GuestCtx and guest memory. Before each
  instruction it checks whether the next PC is in the function table, and if so hands off to
  lifted code. Unimplemented opcodes trap with a full report.
- **Calling conventions**: guest-to-guest calls need no arity knowledge, because ESP is
  emulated and `ret imm16` is lifted as-is. Arity and convention matter only at the
  guest↔native boundary (imports, COM methods, `func_replace`, `call_redirect`). C++
  templates generate typed wrappers there:
  `native_export<cdecl|stdcall|thiscall, R(Args...)>` and
  `guest_call<conv, R>(ctx, addr, args...)`.
- **Native → guest calls** (WndProc, thread entry points, vorbis callbacks, CRT `atexit`, qsort
  comparators) push the arguments plus a sentinel return address, dispatch, and return when the
  sentinel is reached.

### 3.3 Threads and memory ordering

- `CreateThread` creates a host pthread with a guest stack in the guest region, a TEB and its
  own GuestCtx. `CREATE_SUSPENDED` plus `ResumeThread` is supported. `SuspendThread` and
  `TerminateThread` are implemented only if the inventory shows real use.
- Sync objects (critical sections, events, semaphores, mutexes, `WaitForSingleObject[Ex]`) are
  native, and every one of them acts as a full memory barrier.
- **x86 is TSO; ARM is weakly ordered.** The default is plain loads and stores plus barriers at
  sync imports and `lock`-prefixed ops (lowered to host atomics). Functions that poll shared
  flags without locks (expected in CdStream and the audio stream thread) get a manifest marker
  `tso: true`. In those functions the lifter emits acquire/release accesses (`ldapr`/`stlr`).
  The audit happens in M2/M4.

### 3.4 x87

- SA is x87-heavy. Remill's semantics model ST(i) as `x86_fp80`, which arm64 has no type for.
  `sa-recomp` lowers every `x86_fp80` value to `double`. It is stored in the first 8 bytes of
  each 10-byte stack slot, and only real 80-bit memory operands (`fld`/`fstp m80`) convert
  formats. ST(i) is therefore effectively double precision, which differs from real 80-bit x87.
  The interpreter uses the same slot layout, so state passes between the two unchanged.
- Direct3D9 drops the FPU to 24-bit precision on device creation unless
  `D3DCREATE_FPU_PRESERVE` is passed. Check RW's `CreateDevice` flags. If the original ran in
  single precision, lower x87 arithmetic to round-to-float to match it. Either way, track
  `fldcw` and honor the precision-control field in both the lifter and the interpreter.
- An LLVM pass promotes the x87 stack to SSA locals when the stack depth can be proven
  statically per block. Otherwise it falls back to the in-memory stack.
- Rounding-mode-dependent conversions (`fistp`) follow the tracked control word.
- Choose the CPUID feature bits we report on purpose. RW selects SSE math paths at runtime,
  and both paths must be tested.

### 3.5 Exceptions, CRT and process state

- The statically linked MSVC CRT runs unmodified as lifted code. The runtime provides what it
  imports: heap, TLS slots, environment, locale and code-page functions, `GetStartupInfoA`,
  `GetCommandLineA` and std handles.
- SEH: an `fs:[0]` chain in the guest TEB, plus `RaiseException`, `RtlUnwind`,
  `SetUnhandledExceptionFilter` and `UnhandledExceptionFilter`, which cover what the CRT uses.
  Host faults (`SIGSEGV`/`SIGBUS`) are turned into a crash report with the guest PC and guest
  backtrace. They are not delivered to guest SEH unless the inventory shows the game relies on
  that.
- **Virtual registry**: a JSON file. It holds the install path keys and the
  `Shell Folders\Personal` value, which SA likely uses to find "My Documents". That value maps
  to `~/Library/Application Support/MacAndreas/` so user files land there.
- CD and drive checks (`GetLogicalDriveStringsA`, `GetDriveTypeA`, `GetVolumeInformationA`)
  return a fake fixed drive that satisfies any remaining checks.

### 3.6 Graphics

**Why D3D9 first:** RW gets D3D9 through `GetProcAddress`, so a native `d3d9.dll` module lets
the entire RenderWare D3D9 driver and SA's custom pipelines run unmodified. We implement one
documented API (a D3D9 subset), not RW's internal driver contracts plus a D3D9 subset. Native RW
overrides then become an optimization, not a requirement for the first frame.

Phase A (M2–M4) is the D3D9 subset on Metal:
- Fake COM objects: `IDirect3D9`, `IDirect3DDevice9`, vertex/index buffers, textures, surfaces,
  vertex declarations, shaders and queries as needed. Vtables live in guest memory and every
  entry points into the thunk range.
- **Inventory-driven:** every method starts as a logging thunk. Unimplemented methods log once,
  with method name and guest caller, and return a per-method default.
- Adapter and mode enumeration come from launcher settings, so the game sees exactly the
  configured resolution. CAMetalLayer is Retina-aware, and resolution is in pixels.
- Fixed-function state compiles to generated MSL, cached by a state hash. Render, sampler
  and texture-stage state map to `MTLRenderPipelineState` / `MTLDepthStencilState` caches.
- DXT1/3/5 upload directly as BC1/2/3 (Apple Silicon supports BC formats). Paletted and
  legacy formats are converted on upload.
- **Shaders:** a D3D9 SM1.x–3.0 bytecode → MSL translator. Shaders embedded in the exe are
  translated during conversion into a local cache, which is never committed. Others are
  translated at runtime and cached.
- Expected users of the direct device: the building day/night pipeline, vehicle env-map,
  skinning and post effects (colour filter, grain, heat haze, water). Confirm with the
  inventory.
- Device-lost and reset paths are emulated and kept trivial.

Phase B (M10–M11) adds native RW overrides through `func_replace`, chosen by profiling:
- Im3D/Im2D batching (Project2DFX coronas), raster create/lock/upload, and the hottest custom
  pipelines rewritten as native Metal pipelines. librw's backend structure is the design
  reference.

**Movies:** the intro movies are MPEG-1. Confirm what `CoCreateInstance` creates. Preferred:
`func_replace` the game's movie-playback routine with native pl_mpeg (MIT) decoding to a Metal
texture. Fallback: fake DirectShow interfaces. A "skip intro" setting exists from M2.

### 3.7 Audio

- `EAX.DLL#6` returns a native `IDirectSound8`: secondary buffers with `Lock`/`Unlock` ring
  semantics, play cursors, volume, pan and frequency, plus DS3D if used. A CoreAudio
  (`AUAudioUnit`/`AVAudioEngine`) mixer sits underneath.
- EAX property sets (`IKsPropertySet`) start as accepted-but-ignored. Reverb approximation comes
  later behind a setting.
- `vorbisfile.dll` is replaced by native libvorbisfile. `ov_open_callbacks` wraps the guest
  read/seek/tell/close callbacks through `guest_call`. The audio stream thread is a real guest
  thread.

### 3.8 Input

- `DirectInput8Create` returns native mouse (raw deltas from NSEvent) and joystick devices.
  Keyboard input arrives as synthesized `WM_KEYDOWN`/`WM_CHAR` plus `GetKeyState`. Confirm
  which path SA reads keyboard from.
- GameController.framework devices appear as a DInput8 joystick with an Xbox-style layout.
- Cursor: `ClipCursor`, `ShowCursor` and `SetCursorPos` map to cursor hide, warp and
  `CGAssociateMouseAndMouseCursorPosition`.

### 3.9 VFS

- Case-insensitive lookup over the game dir, with a cached directory index. `\` maps to `/`.
  Drive letters map to the game root.
- Overlay order: user dir (saves, `gta_sa.set`) → baked mod data → game dir.
- Overlapped I/O (`ReadFile` + `OVERLAPPED` + `GetOverlappedResult`) for CdStream is
  implemented on a host I/O worker with real completion semantics.
- `CreateFileMappingA`/`MapViewOfFile` map into the guest region.

### 3.10 Hook manifest

YAML, one entry per patch, as in the original plan, plus these fields:

```yaml
- addr: 0x00000000
  kind: byte_patch | nop | data_poke | call_redirect | mid_hook | func_replace | operand_slot | data_slot |
        vtable_redirect | imm_redirect
  source: silentpatch | widescreen | project2dfx | native
  group: <fix-name>
  target: <native symbol>
  bytes: <hex>
  len: <n>
  tso: <bool, lifter memory-ordering marker; applies to the function at addr>
  setting: <settings key that enables this group, optional>
  resume: <addresses a mid_hook may resume at, or +N offsets from each addr; lifted as trace heads, optional>
  site: <inventory row id this entry ports, when it hooks a different instruction, optional>
  notes: <intent / upstream changelog ref>
```

Build-time checks:
- Overlapping entries.
- Patches that don't start on an instruction boundary. Boundaries come from the analysis data.
- Mod patches inside a `native` `func_replace` function, or inside RW/D3D paths replaced
  natively.
- `mid_hook` targets inside a block the lifter can't split.
- Settings-gated groups compile to runtime checks for `data_poke`/`func_replace`. For
  lift-time `byte_patch`, the lifter generates both variants and selects one at startup.

`addr` may list several addresses, separated by commas. A list may continue on more-indented
lines after a trailing comma. Each address becomes its own entry with the other fields shared.

`operand_slot` gives an x87 load or arithmetic instruction with an absolute m32 operand (`fld`,
`fadd`, `fmul` and so on, from a constant) its own value. The lifter points the operand at a
per-instruction slot (`0xE8000000 + 4 * addr`). The host fills the slot from the native slot
function `target`, which is given the constant's value at startup. When the entry's setting is
off, the slot copies the original constant. `data_slot` sets a float in the game's data to the
value of a slot function, when its setting is on. The host refreshes both kinds once per frame.
The Widescreen Fix uses them to change the HUD's scale constants per instruction, as the mod
does by pointing each operand at its own variable.

`vtable_redirect` points a vtable slot (`addr`, in the game's data) at a native, as a mod does when it
replaces a virtual function and keeps the old pointer. The host registers the native at
`0xE0000000 + addr`, writes that address into the slot and keeps the old pointer for
`redirect_original()`, so the native is written like a `call_redirect` target. When the entry's setting
is off, the slot keeps the game's function.

`imm_redirect` does the same for a function pointer in code: `addr` is the 32-bit immediate of a
`push imm32` or `mov r/m32, imm32` (SilentPatch writes its render callbacks there). The lifter writes
`0xE0000000 + addr` into the immediate, and the host registers the native there with the old pointer
for `redirect_original()`. Because the operand is fixed at lift time, an entry whose setting is off
still goes through the redirect address, which then calls the original.

`mid_hook` targets receive `GuestCtx&`. Naked asm stubs from the mods are hand-translated
into functions that take `GuestCtx&`.

### 3.11 Conversion pipeline and artifacts

```
dev only:   gta_sa.exe ──Ghidra headless──▶ analysis/sa-1.0us/*.json   (committed, address-only)
user/dev:   gta_sa.exe + analysis + manifest ──sa-recomp──▶ ~/Library/Application Support/MacAndreas/build/
                ├ libgta_sa_native.dylib   (ad-hoc signed)
                ├ functable.bin
                ├ shaders.cache            (translated from embedded D3D9 bytecode)
                └ stamp.json               (hash of exe + analysis + manifest + sa-recomp version)
```

- `sa-recomp` is one native C++ binary with Remill and LLVM linked as libraries. It shards the
  functions, lifts and optimizes in parallel across all cores, and links with bundled `lld`.
  There is no Java and no system toolchain dependency.
- If the stamp doesn't match, the launcher reconverts.
- **Code signing:** the generated dylib is ad-hoc signed. The notarized `sa-host` runs with
  the hardened runtime and `com.apple.security.cs.disable-library-validation` so it can load
  the locally built dylib. The in-house interpreter needs no JIT entitlement.
- Analysis files: `functions.json` (start, end, convention, arity if known, noreturn),
  `jumptables.json`, `indirect_targets.json`, and `overrides.yaml` (hand fixes for
  functions Ghidra gets wrong, especially in `.text`(2) and `.HOODLUM`).

---

## 4. Milestones

```mermaid
graph LR
  M0[M0 Foundations] --> M1[M1 Lift & boot]
  M1 --> M2[M2 First frame]
  M2 --> M3[M3 Menu]
  M3 --> M4[M4 In-world]
  M4 --> M5[M5 Audio]
  M5 --> M6[M6 Playable vanilla]
  M0 -.inventory can start.-> M7
  M6 --> M7[M7 Mod infrastructure]
  M7 --> M8[M8 Widescreen Fix]
  M8 --> M9[M9 SilentPatch]
  M9 --> M10[M10 Project2DFX]
  M10 --> M11[M11 Perf & polish]
  M11 --> M12[M12 Release]
```

Parallel tracks once M1 lands: **recomp/runtime** (lifter quality, interpreter, Win32),
**graphics** (D3D9 layer, shader translator), **audio/input**, and **mods** (inventory and
triage, which needs only the mod sources). They meet at each milestone's exit criteria.

### M0 — Foundations

Goal: the repo, the toolchain, and the guarantee that no game bytes are ever committed.

Status: **complete**. Remill (pinned commit `66c488c`) builds unpatched against Homebrew LLVM
22.1 via `tools/recomp/setup-remill.sh`. The `game.lift_smoke` test lifts two 1.0 US functions
at test time and runs them natively. A stack-argument read-modify-write and a global-table
lookup at original `.data` addresses both give correct results.

The first export found 20,810 functions, 1,248 computed jumps and 9,001 computed call sites,
and confirmed that `.text` thunks into `.HOODLUM` (for example `0x00401000` → `0x01562980`).

Findings carried into M1:
- Remill optimizes under the i386 datalayout, and LLVM folds State accesses into raw byte
  offsets. Retargeting to arm64 is sound only because `%struct.State` has an identical layout
  on both (3,504 bytes, 167 leaf fields). `state-layout-check` proves this, and `sa-recomp`
  must run it on every build.
- Lifted code adds `DSBASE`/`SSBASE` to every memory access, so GuestCtx must be
  zero-initialized with flat segment bases. Only FS points at the TEB.
- Ghidra recognizes the calling convention for only ~4% of functions. As planned, this matters
  only at guest↔native boundaries.

Work:
- Repo layout (§6). CMake + Ninja superbuild for C++. SwiftPM for the launcher.
- `LICENSE` (GPL-3.0-or-later) and `THIRD_PARTY_NOTICES.md` (SilentPatch MIT, WidescreenFixesPack
  GPL-3.0, Project2DFX GPL-3.0, Remill Apache-2.0, LLVM Apache-2.0 WITH LLVM-exception,
  Zydis MIT, libvorbis/libogg BSD-3, pl_mpeg MIT).
- `.gitignore`: `gamefiles/`, `*.exe`, `*.dll`, `*.img`, `build/`, `out/`, `*.ll`, `*.bc`, `*.o`,
  `*.dylib`, shader caches.
- **Repo guard** as a pre-commit hook and CI job. It rejects PE/MZ files, LLVM bitcode/IR,
  Mach-O objects, files over a size threshold, and long hex/base64 blobs in `analysis/`.
- `sa-verify`: size + SHA-256 check. It recognizes known Steam/RGL hashes and gives specific
  downgrade guidance.
- `pe-inspect`: sections, imports (with ordinal names), resources and entry point. Its output
  checks §2.
- Ghidra 12.2 headless scripts that export `analysis/sa-1.0us/*.json`
  deterministically (sorted, stable).
- Remill + its matching LLVM version built and pinned. Smoke test: lift one leaf function,
  map the image by hand, call it from a test, and compare against known outputs.

Exit criteria:
- `sa-verify` accepts the 1.0 US exe and rejects a one-byte-modified copy and the Steam exe.
- Running the Ghidra export twice produces byte-identical JSON.
- The repo guard blocks a test commit containing the exe and a test `.ll`.
- A single lifted function returns correct results from a host test.

### M1 — Lift & boot

Goal: the whole exe lifted and running natively from entry point to `WinMain`.

Status: **exit criteria met** (details below). A full `sa-recomp build` lifts 76.5k traces from
71.8k roots into 189 shards with 0 unresolved stubs, and compiles and links them in about
11 minutes on a 10-core M-series machine. Booting runs about 387k API calls:
- The CRT runs: heap, TLS, locale, C and C++ static initializers.
- WinMain loads `AMERICAN.GXT`, opens `MODELS\GTA3.IMG` and starts the CdStream thread.
- It passes the DirectX/DirectSound probes and calls `RegisterClassA` and `CreateWindowExA`.
  GetDXVersion is replaced natively by the first `native` manifest entry
  (`manifest/native.yaml`), so the D3D8/dpnhpast/D3D9 probe loads no longer happen.
  WM_NCCREATE/WM_CREATE run through the guest WndProc.
- It stops at the delay-loaded `DirectInput8Create`, which is M5 input work. M2 needs at least a
  DirectInput shell to reach `Direct3DCreate9`.

How root discovery works:
- Sources: Ghidra functions, the entry point, aligned data pointers into code, and code
  immediates (`push imm32`, `mov r32, imm32`, `mov r/m32, imm32`) in every executable section.
- Jump-table targets are roots too. This Remill version never calls `ForEachDevirtualizedTarget`,
  so `jmp [table]` is always a runtime dispatch, and each case block is its own trace.
- Ghidra resolved 1,046 tables. A `jmp [4*reg+table]` scan recovers 7 more, including two-level
  switches that the protection spliced through `.text`.
- The remaining 195 computed jumps are `jmp reg` or `jmp [reg+N]` tail calls through function
  pointers. `sa-recomp` lists them in `out/unresolved_jumps.txt`.
- `tools/recomp/interp_triage.py` turns `[INTERP]` log lines into entries in
  `analysis/sa-1.0us/extra_roots.txt`, which `sa-recomp` reads as roots.
- The only `[INTERP]` seen during boot was the WndProc at `0x00747eb0`. Its address is passed with
  `mov [esp+x], imm32`, which the scan now covers. The final boot has zero `[INTERP]` hits.

Findings:
- **The protection region produces one runaway trace.** `sub_012cc7d0` in the second `.text`
  section lifts to 6.9M instructions, and GVN never finishes on it. Traces over 200k
  instructions are compiled `optnone`. The new pass manager honors `optnone` only when
  `StandardInstrumentations` is registered.
- **`.HOODLUM` uses `push addr; ret` as a jump**, sometimes into an import thunk, and Remill lifts
  every `ret` as a return. Every guest call therefore ends in `finish_call`: while `eip` is not
  the expected return address, it keeps dispatching at `eip`. Without this, callee-saved
  registers were corrupted and WinMain skipped `GTA3.IMG` and DirectInput.
- **Direct guest calls are routed through `__remill_function_call`** by an IR pass, not emitted
  as direct LLVM calls. This is what makes these work:
  - SEH frame matching.
  - Native `func_replace`.
  - Interpreter handoff in both directions.
  - The `ret`-as-jump repair above.

  Trace lookup is an open-addressing table. Tail jumps between traces stay direct.
- Lifted objects carry unwind tables. A non-local SEH transfer throws a host C++ `GuestUnwind`,
  and the `__remill_function_call` frame whose guest return address and stack slot match catches
  it. The cost on the normal path is zero.
- The game reads `Shell Folders\Personal` for its user-files folder. The in-memory virtual
  registry maps it to `C:\Users\Player\Documents`, which is
  `~/Library/Application Support/MacAndreas/Documents` on the host.

Deviations from the plan:
- `analysis/` is local-only (gitignored). Building from source therefore needs a local Ghidra
  12.2 export, and `extra_roots.txt` is per-developer. The hybrid plan assumed committed analysis
  data. Open question: commit address-only data after a licensing check, or keep it local.
- File I/O, sync objects and threads (CreateThread on pthreads with 64 MB host stacks) were
  pulled forward from M2, because WinMain needs them before the window path. So were a minimal
  virtual registry, the ddraw/dsound COM shells, and ole32 stubs.
- Win32 code lives in `host/src/win32` rather than `/runtime/win32`, and the manifest engine
  lives in `runtime/manifest`.
- The function table is a hash table exported from the dylib (`ma_functable`), not a separate
  `functable.bin`.

Not done in M1, carried forward:
- The debug shadow stack and community symbol map in the crash reporter.
- Consuming `tso` markers in the lifter.
- Lowering `call_redirect` and `mid_hook` (M7). Both are parsed and checked, but rejected at lift
  time.
- Honoring x87 precision control.
- Most `ApiDef`s still decode arguments by hand. New code should use `native_export`.

Work:
- Guest region, section mapping, protections, TEB/PEB and the guest-VA allocator (§3.1).
- Whole-program `sa-recomp`: shard, lift, run custom passes (inline memory access with
  `guest_base`, flag dead-store elimination, x87 promotion, atomics), `-O2`, link the dylib,
  emit `functable.bin`.
- Analysis hardening for `.text`(2), `.init` and `.HOODLUM`: entry-reachable plus data-referenced
  code, with `overrides.yaml` for obfuscated thunks and overlapping instructions.
- Interpreter v0 (Zydis + integer core + x87 core) with lifted-code handoff. `[INTERP]` logging
  feeds a script that proposes new `functions.json` entries.
- Native thunk range, import resolver, and every import as a logging trap first.
  `LoadLibraryA`/`GetProcAddress` serve the native module table.
- ABI templates (§3.2) and the native→guest call bridge.
- **Manifest engine core**: parser, all six kinds, basic conflict checks. It is used here by
  `native` entries.
- CRT bring-up: heap, TLS, environment, locale/code pages, SEH (§3.5), timing.
- Crash reporter: guest PC, guest backtrace (debug shadow stack), and local-only symbolization
  from a community symbol map the developer supplies (not committed until its license is
  checked).

Exit criteria:
- Every function in `functions.json` lifts and links. Lift failures are listed with a reason.
- CRT init completes and execution reaches `WinMain` and the window-creation path.
- Every `[INTERP]` address seen during boot is either added to the analysis or documented.
- Unit tests pass for the calling-convention wrappers, the native→guest bridge, SEH chain walk
  and the interpreter/lifted handoff.

### M2 — First frame

Goal: splash and loading screens rendered by lifted RW through the native D3D9 layer.

Status: **exit criteria met** (details below). `sa-host` boots to the
main menu through lifted RenderWare and the native D3D9 layer. The EAX and nVidia splashes, the
legal/title screen and the main menu render at the configured 1280x720 and match the original's
layout and colors. Scripted key presses then take the menu through Start Game → New Game to the
splash-art loading screen. A 1,350-frame run of that path under `MTL_DEBUG_LAYER=1` reports no
Metal validation errors, no unimplemented COM methods and no skipped shader draws.

What is in place:
- **Window**: an AppKit `NSWindow` whose view is a `CAMetalLayer` sized in pixels (Retina-aware).
  The guest main thread is the host main thread, so `PeekMessageA` and `Present` pump AppKit
  events. A real message queue backs `PeekMessageA`/`PostQuitMessage`. Key-window changes post
  `WM_ACTIVATEAPP`, `WM_ACTIVATE`, `WM_SETFOCUS`/`WM_KILLFOCUS`; the close button posts `WM_CLOSE`.
- **D3D9 subset v0** (`host/src/gfx`): `IDirect3D9`, the device, 2D and cube textures, surfaces,
  vertex/index buffers, vertex declarations, shaders (stored only), queries and the swap chain.
  Every interface has a complete vtable. Methods without an implementation log once with the
  guest caller and return a per-method default.
- **Fixed-function pipeline**: device state compiles to a `PipelineKey`. MSL is generated per
  shader key and cached. It covers pretransformed and transformed vertices with the D3D9
  half-pixel offset, 8 lights with material sources, texture coordinate generation and transforms,
  every texture stage op and argument modifier, specular, vertex and table fog, and alpha test.
  Blend, depth/stencil and sampler state map to cached Metal objects.
- Vertex and index data are copied per draw into a staging ring, so `Lock` never waits on the GPU.
  Textures keep a guest-memory copy. Non-BGRA8 formats are converted on unlock, and DXT1/3/5
  upload as BC1/2/3.
- `--resolution WxH` sets the one mode the adapter reports. `--capture-dir` with
  `--capture-frames N,M,…|all` writes PNGs of presented frames, and `--exit-after-frames N` ends a
  run.
- **Keyboard**: AppKit key and modifier events map to Win32 virtual keys and set-1 scan codes
  (`host/src/input/keys.cpp`), post `WM_KEYDOWN`/`WM_KEYUP` (`WM_SYSKEY*` with Alt and for F10) with
  the repeat, extended and transition bits, and update `GetKeyState` (including left/right
  modifiers and toggles) and the DirectInput keyboard state. Cmd shortcuts stay with macOS. SA reads
  the keyboard only through `WM_KEY*` and `GetKeyState(VK_LSHIFT/VK_RSHIFT)`.
- `--keys FRAME[m]:KEY[+|-],…` scripts key presses for unattended runs: a tap at a presented game
  frame (or movie frame with `m`), or a press (`+`)/release (`-`). A tap is released after the game
  has run one frame with the key down, counted by empty `PeekMessageA` calls.
- A DirectInput8 shell (mouse and keyboard devices), a silent DirectSound8 (buffers with
  `Lock`/`Unlock`, play cursors that advance in real time, 3D buffer and listener facets) and a
  fake DirectShow filter graph that plays the intro movies.

Findings:
- On first run, psSelectDevice insists on an 800x600x32 mode (`defaultFullscreenRes`,
  `0x008d6218`), and `GcurSelVM` (`0x008d6220`) starts at -1. Two `native` data pokes clear the flag
  and start at RW mode 1, the first exclusive mode, so the game takes the configured mode.
- **Thunk table race.** `invoke_thunk` kept a pointer into a `std::vector` of thunks after
  releasing its lock. COM interfaces register thunks lazily on the main thread, so a reallocation
  during sound initialization could pull the table out from under the CdStream thread (an
  intermittent crash at host address `0x23`). The table is now a `std::deque`.
- Movie skip: the logo and intro states advance on a just-pressed Enter, Space, Escape or Tab, or
  when any pad-0 button is down. Pad-0 controller state is computed from the previous keyboard
  snapshot, and Return/Space are bound to pad buttons, so one Return press during `Logo.mpg` also
  skips `GTAtitles.mpg`; Escape skips one movie at a time. This follows from the original's logic,
  not the host.
- **Indirect jumps recursed on the host stack.** `__remill_jump` called `dispatch`, so the game
  loop, which never returns, overflowed the 8 MB main stack after about 600 frames. `__remill_jump`
  now records a thread-local pending target, and `dispatch` loops on it.
- SecuROM anti-debug code in `.text`(2) (`push ss; pop ss; pushfd`, a trap-flag check) lifts to
  emulate-instruction hypercalls and decode-error traces. The hypercall now emulates segment
  push/pop. `__remill_error` falls back to the interpreter instead of aborting, and the interpreter
  handles segment registers.
- `CreateDevice` passes behavior `0x50` (pure device, hardware vertex processing) without
  `D3DCREATE_FPU_PRESERVE`. The original therefore runs x87 in single precision from device
  creation on (§3.4, M4).
- SA needs a DirectSound device: when `DirectSoundCreate8` fails, it crashes in the audio engine.
  `LoadLibraryA` and `GetModuleHandleA` now fail for modules without a native implementation
  (`wmvcore.dll`, `QuickTime.qts`, `mscoree.dll`). That disables WMA/QuickTime user tracks, as on a
  machine without them.
- The game creates 6 pixel shaders at startup but draws the boot and menu screens with fixed
  function only. No shader draws happen up to the main menu.

Movie path decision: the movie code drives DirectShow (`CLSID_FilterGraph`: `IGraphBuilder::RenderFile`,
`IMediaControl`, `IMediaEventEx` notifying with `WM_GRAPHNOTIFY` `0x40d`, `IMediaSeeking`,
`IVideoWindow`). It waits for `EC_COMPLETE` or a key press, and it doesn't call `Present` while a
movie plays. MacAndreas answers with a fake filter graph instead of replacing the movie routine
with `func_replace`. The graph decodes the MPEG-1 file with pl_mpeg (vendored in
`third_party/pl_mpeg`). `PeekMessageA` advances it by wall-clock time, and each new frame is
presented letterboxed through the Metal layer. At the end of the stream the graph posts
`EC_COMPLETE`. `Logo.mpg` (15.3 s) and `GTAtitles.mpg` (88.7 s) play to the end at 30 fps. Movie
audio is decoded later, with the CoreAudio output in M5. `--skip-intro` completes the graph at
once. Movie frames are captured as `movie_NNNNN.png` with the same `--capture-frames` numbers,
counted separately from game frames.

CdStream TSO audit (first pass):
- The request queue (ring at `0x008e3fec`, head `0x008e3ff0`, tail `0x008e3ff4`) is ordered by the
  semaphore at `0x008e4004`. The native semaphores are full barriers, so the queue is safe.
- Channel handshake: CdStreamThread (`0x00406560`) does `ReadFile; status = …; nSectorsToRead = 0;
  if (bLocked) ReleaseSemaphore`. CdStreamSync (`0x0156cd80`, in `.HOODLUM`) does
  `if (nSectorsToRead) { bLocked = 1; WaitForSingleObject }`. This store→load handshake can
  already lose a wakeup under x86 TSO. On ARM, the plain lifted stores of `status`,
  `nSectorsToRead` and `bLocked` can also reorder with each other and with the buffer data.
- CdStreamGetStatus (`0x01563850`) and the streaming code poll channel status without locks.
- Mitigation now: native `ReadFile` and `GetOverlappedResult` issue a `seq_cst` fence, so buffer
  data is published before the thread's flag stores. Planned for M4: `tso: true` on CdStreamThread,
  CdStreamSync and CdStreamGetStatus once the lifter consumes markers, or a native CdStreamSync
  with a proper wait (SilentPatch also replaces it, M9).

D3D9 coverage up to the main menu: 59 entry points are called, and all are implemented:
`Direct3DCreate9`; `IDirect3D9` adapter count/identifier/modes/display mode, `CheckDeviceType`,
`CheckDeviceFormat`, `CheckDeviceMultiSampleType`, `CheckDepthStencilMatch`, `GetDeviceCaps`,
`CreateDevice`; the device's `Reset`, `TestCooperativeLevel`, `EvictManagedResources`, scene,
`Clear`, `Present`, gamma ramp, render target and depth surface get/set, viewport, transform,
render/texture-stage/sampler state, textures, streams, indices, declarations, shaders,
`DrawPrimitive`, `DrawIndexedPrimitive`, and texture, buffer and declaration creation; texture
level desc/count/surface; surface and buffer lock/unlock. Planned defaults:
- `SetGammaRamp` is accepted and ignored (brightness is M3).
- State blocks, `ProcessVertices`, patches, `CreateAdditionalSwapChain` and `GetPaletteEntries` return
  `D3DERR_INVALIDCALL`/`NOTAVAILABLE`, and volume textures are refused.
- User clip planes and stream frequency are stored and logged but not applied.

Deviations from the plan:
- Graphics, platform and input code live in `host/src/gfx`, `host/src/platform` and `host/src/input`
  rather than `/runtime/gfx`, for the same reason as `host/src/win32` in M1.
- The silent DirectSound and the DirectInput shell were pulled forward from M5/M3, because
  initialization needs them.
- The splash-art loading screen appears only after Start Game → New Game. At boot, SA shows the
  legal/title screen while it loads. Keyboard input was pulled forward from M3 to reach it.
- Screenshots were compared against the original by eye. The M7 reference harness doesn't exist
  yet.

Not done in M2, carried forward:
- Movie audio (done in M5).
- The shader translator (M4). Draws with a vertex or pixel shader set are skipped and counted.
- Mouse `WM_*` messages, cursor capture and menu mouse use (M3). Mouse deltas and buttons are
  recorded for DirectInput and `GetKeyState`.
- New game crashed after `data\script\main.scm` loaded (`0x004aa148`). Fixed in M3: the guest heap
  moved a shrinking `realloc` (see M3 findings).
- Pause on focus loss: the activation messages are delivered, but the behavior is not verified (M3).
- MSAA, multiple render targets, volume textures and fixed-function vertex blending.
- TSO markers (M4).

Work:
- Window: NSWindow + CAMetalLayer. `RegisterClassA`/`CreateWindowExA`, guest WndProc dispatch,
  message queue (`PeekMessageA`/`DispatchMessageA`), activation and focus messages.
- D3D9 subset v0 (§3.6): device creation, buffers, textures, FF pipeline generator, `Present`,
  method-coverage logging.
- VFS v0 and file I/O, including overlapped reads. CdStream thread on real host threads.
- Sync primitives and `Sleep`. `timeGetTime`/QPC.
- First TSO audit of CdStream (§3.3).
- Movie path decision (§3.6). "Skip intro" setting.

Exit criteria:
- Legal/splash screens and the loading screen render at the configured resolution, matching
  reference screenshots for layout and color.
- Intro movies play, or are skipped by setting.
- No Metal API validation errors. Every D3D9 method called is either implemented or listed
  with a planned default.

### M3 — Menu

Goal: the main menu is fully usable and settings persist.

Status: **exit criteria met** (details below). Every page of the main menu was reached with
scripted keyboard and mouse input and captured at 1280x720 or 1280x800. Display, audio and
control settings changed in the menu are read back after a restart. Start Game → New Game loads
into the opening cutscene (Los Santos airport, then the Grove Street drive) and keeps running.

What is in place:
- **Mouse**: AppKit pointer events become `WM_MOUSEMOVE` and button messages in client pixels of
  the game's resolution, however the window is scaled. `WM_MOUSEWHEEL` comes in 120-unit notches,
  with trackpad deltas accumulated. Mouse moves are coalesced in the queue as Windows does.
  `ShowCursor` hides the macOS cursor inside the window while the game draws its own. `SetCursorPos`
  warps the real cursor when the window is key and the pointer is inside it, and posts the move.
  The menu reads its pointer from `WM_MOUSEMOVE` (`0xba6828`/`0xba682c`) and clicks from
  DirectInput.
- `--keys` scripts mouse steps too: `lbutton`/`rbutton`/`mbutton` (with `@X:Y`, `+`/`-`), `move@X:Y`,
  `wheelup`/`wheeldown`. `focus` and `blur` post the window's activation messages, and `Ns` runs a
  step N seconds after start, for a paused game that presents no frames.
- **Mode list**: the adapter lists the configured resolution first, then standard modes that fit the
  main display (`--modes WxH,…` overrides them). `GetVideoModeList` (`0x745af0`) is replaced natively:
  every exclusive mode of at least 640x480 gets a label, and the menu's Resolution option cycles
  through exactly that list. Changing it resets the device and resizes the window.
- **Settings**: SA keeps every setting in `gta_sa.set`. It reads the registry only for the
  "My Documents" path, so the virtual registry needs no persistence. The user files live in
  `~/Library/Application Support/MacAndreas/Documents/GTA San Andreas User Files`.
- **`vorbisfile.dll`**: native libvorbisfile behind the game's read/seek/close/tell callbacks, which
  run as guest calls on the calling thread. Decoded PCM is written straight into the guest buffer;
  `ov_info` returns a guest copy of `vorbis_info`.
- Pause on focus loss is the original's: `WM_KILLFOCUS` clears `ForegroundApp` (`0x8d621c`) and the
  main loop then sleeps 100 ms per pass without running a frame, in the menu and in game.
  `WM_ACTIVATE` also pauses and resumes the audio engine.

Findings:
- **The guest heap moved shrinking reallocs.** The effects pool (`FxMemoryPool_c`, 1 MB) is shrunk
  with `realloc` after `effects.fxp` loads, and the effect blueprints keep pointers into it. Windows
  shrinks in place; `GuestAllocator::realloc` moved the block to a smaller size class and freed the
  old one, so a later 1.2 MB audio allocation zeroed the blueprints and `FxManager_c::Update`
  crashed on the first game frame. Shrinks, and growth within a block's capacity, now stay in place,
  and each chunk records its size class.
- Vanilla `GetVideoModeList` labels only 4:3, 5:4, 5:3 and 16:10 modes, never 16:9, and only those
  leaving 45 MB of video memory. Unlabeled modes are NULL, and Display → Advanced crashed copying
  the NULL label of the selected 1280x720 mode.
- Clearing `defaultFullscreenRes` (M2) also skipped the first-run copy of `GcurSelVM` into the menu's
  `m_nPrefsVideoMode`/`m_nDisplayVideoMode` (`0x748c65`), which stayed at mode 0, the windowed
  desktop mode. A third `display-mode` poke starts both at mode 1. A `gta_sa.set` saved before
  this fix holds mode 0 and makes the game reset into windowed mode 0; delete it once.
- `LoadSettings` reads the saved mode into `m_nDisplayVideoMode` (`+0xd8`), while `SaveSettings`
  writes `m_nPrefsVideoMode` (`+0xd4`), at byte 2870 after the 2,832-byte controls block.
- The Audio page's radio station is not read from `gta_sa.set`; the audio engine picks it at boot.
- WinMain leaves its loop through the menu's quit flag (`0xba67a5`), not `WM_QUIT`, and returns
  `msg.wParam` of the last message peeked. Quitting with Return exits with code 13, as on Windows.
- The audio thread procedure (`0x4eee90`) ends with `ret`, not `ret 4`, so the host logs a 4-byte
  stack mismatch when it exits. The thread ends there, so it is harmless.

Deviations from the plan:
- libvorbis and libogg are linked statically from Homebrew rather than a pinned submodule. Vendoring
  them is release work (M12).
- `GetVideoModeList` is replaced in M3 rather than left to the Widescreen Fix (M8), because the
  menu must offer the host's 16:9 modes without crashing. The FOV and HUD fixes remain M8.

Not done in M3, carried forward:
- `ClipCursor` and relative mouse capture for in-game mouse look (M6).
- Character skinning and other shader draws are skipped until the shader translator (M4), so the
  cutscene renders without people.
- `IDirectSoundBuffer8::SetFX` returns `E_NOTIMPL` (done in M5).

Work:
- Keyboard and mouse input (§3.8): mouse WM_* synthesis, DInput8 mouse, cursor capture. Keyboard
  `WM_KEY*`, `GetKeyState` and the DInput8 keyboard state landed in M2.
- Virtual registry and the user-dir mapping. `gta_sa.set` read/write.
- Display settings are constrained to the launcher-provided mode list.
- Pause on focus loss, matching original behavior.

Exit criteria:
- Every menu page is reachable by keyboard and mouse.
- Display, audio and control settings survive a restart.
- "Start new game" begins loading.

### M4 — In-world

Goal: free roam in Los Santos with correct rendering and physics.

Status: **exit criteria met except vehicle handling against a reference**. `tools/route/m4_route.py`
starts a new game and plays a 10-minute route from Grove Street:
- the opening mission's BMX into East Los Santos
- the Rhino (`AIWPRTON`) through the streets, with mouse look while driving
- a walk and a sprint, then the jetpack (`ROCKETMAN`) over the city
- the faster clock (`YSOHNUL`) through several days and nights

It runs 24,450 frames with no crash, zero `[INTERP]` hits and zero skipped shader draws. Handling
by feel and on recorded checkpoints still needs a person and the M7 reference harness.

What is in place:
- **Shader translator** (`host/src/gfx/sm.cpp`, `sm_msl.cpp`): parses vs/ps 1.1–2.0 bytecode and
  emits MSL functions that plug into the fixed-function pipeline key.
  - Constants live in full `VertexConstants`/`PixelConstants` blocks (buffers 9 and 1). ps1.x
    constants are clamped.
  - Fog and alpha test are applied after the pixel shader.
  - `a0` follows SM1 floor and SM2 round. Relative addressing, loops and `if`/`rep` are handled.
  - SM3, predication, MRT and depth output are rejected with a `[SHADER]` log. SA never uses them.
  - Every shader draw on the route is translated (0 skipped). Skinned peds, buildings and
    vehicles look right in the captures. Water and post effects have not been compared against
    the original.
  - `--dump-shaders DIR` writes disassembly.
- **x87 precision** (§3.4): `CreateDevice` without `D3DCREATE_FPU_PRESERVE` sets PC=24 on the
  calling thread, as d3d9 does.
  - `FLDCW`/`FNSTCW` semantics are rebuilt so the control word round-trips. Remill cleared the
    precision bits.
  - The lowered `fadd`/`fsub`/`fmul`/`fdiv`/`fsqrt` (including `sqrtl`) round to float when PC=0, as
    does the interpreter.
  - The arithmetic inside `fyl2x`, `fyl2xp1`, `f2xm1`, `fpatan`, `fprem`/`fprem1` and `fscale` is
    exempt, through `ma_x87_exact_*` markers, because real hardware runs those at full precision.
  - The mismatch made `CTrain::ProcessControl` spin forever after the intro.
- **x87 correctness found by the differential tests:**
  - Remill's `fpatan` computed `atan(st1/st0)` and lost the quadrant. It is now `atan2l`.
  - `fist`/`fistp` of an out-of-range value or NaN stores the integer indefinite (`0x80000000`),
    not ARM's saturated value.
  - `fnstsw` reports C0–C3 and TOP only, in both implementations, because remill's sticky exception
    flags are ad hoc and compiled code never reads them.
  - Remill's 80-bit load (`fld m80`) rebuilds the value as an `i80` and stores those bits into
    the stack slot. That clashed with the double-in-slot lowering: every `fld tbyte` pushed the
    80-bit mantissa read as a double. The CRT's x87 `asin` returned garbage for exactly 1.0,
    `CBike::ProcessControl` divided by it, and CJ and the BMX became NaN on the first frame of the
    ride. `i80` loads and stores to memory now convert through doubles, and
    `__remill_read_memory_f80` passes the real sign and exponent.
  - The interpreter's `fxch st(i)` and `fcomi st, st(i)` used st(1) whenever Zydis exposed one
    visible operand. They now use ModRM. `fyl2x`, `fyl2xp1`, `f2xm1`, `fxtract` and `xlat` were added,
    and `fprem` sets the quotient bits.
- **Differential tests** (`sa-host --diff-test`): each function runs lifted and interpreted in
  forked children from the same randomized registers, stack, buffers and control words (double,
  single, extended). Registers, the x87 stack and every written byte are compared.
  - On the 360 leaf x87 functions from `tools/recomp/x87_candidates.py`: 337 agree, 0 differ and
    23 are inconclusive (both fault or time out on random pointers). The first run had 58
    differences.
  - On the 267 functions in the CRT math range, with x87 arguments and callees interpreted
    (`--diff-x87-args`, `--diff-deep`), the remaining differences are interpreter gaps only: the
    CRT's SSE2 math paths. The interpreter has no SSE2.
- **TSO markers**: `kind: marker` manifest entries with `tso: true` are written to `tso.txt`.
  `compile` then fences every guest load (acquire) and store (release) in those traces.
  CdStreamThread, CdStreamSync and CdStreamGetStatus carry them (70, 38 and 24 barriers). A marker
  that names no lifted trace fails the build.
- **Interpreter coverage**: zero on the route.
  - A raw scan of `.text` finds 70 jump tables the analysis missed (1,092 targets).
  - A small evaluator recognizes the 48 SecuROM call/jump stubs that compute their target and
    return point from two data words (`add`/`sub`/`xor`/`adc`/`sbb`, `not`, `rol`/`ror` by `cl`).
    It lifts both.
  - `__remill_error` falls back to the interpreter.
- **Input**: a tapped key stays down until a frame has been presented, so typed cheats register.
  `look@DX:DY` turns the camera. `--ignore-focus-loss` keeps unattended runs from pausing.
- **Debugging**:
  - `--interpret-range LO-HI` bisects lifting bugs.
  - `--trap-nan` logs the first interpreted instruction that produces a NaN.
  - `--watch ADDR` logs guest values per frame.
  - `--trace-pc` and `--interpret` target single traces.

Findings:
- The route is not frame-deterministic. SA steps physics by measured time, so a loaded machine
  changes where the player ends up. Run routes on an idle machine. A fixed-timestep mode would
  make them reproducible.
- Direct calls between lifted functions bypass the dispatcher, so `--interpret-range` cannot
  force everything in a range into the interpreter.
- The CRT selects its SSE2 math functions at startup (`0xc9c400`). Those run lifted; the
  interpreter cannot cross-check them.

Not done in M4, carried forward:
- Vehicle handling against a reference by feel and on recorded checkpoints (M7 harness).
- Interpreter SSE/SSE2 support, for differential tests of the CRT's SSE2 math.
- A fixed-timestep option for reproducible routes.

Work:
- D3D9 shader translator and the custom pipelines: buildings day/night, vehicle env-map,
  skinning, post effects, water.
- Streaming stress: IMG reads under load, overlapped I/O correctness, final TSO markers.
- x87 fidelity: differential tests (lifted vs interpreter) on vector/matrix/physics routines.
  Precision-control handling per §3.4.
- Interpreter coverage is driven to zero for this route.

Exit criteria:
- New game → Grove Street, then a scripted 10-minute route: walk, sprint, drive, bike, fly, and
  watch day and night pass.
- Mouse camera works. Vehicle handling matches the reference by feel and on recorded checkpoints.
- No crash and zero `[INTERP]` hits on the route.

### M5 — Audio

Goal: full audio support. Controller support moved to M11.

Status: **audio exit criteria met except two checks that need a person or the reference**: 3D
positioning against the reference (M7 harness) and lip-sync by eye. `tools/route/m5_route.py`
cycles every radio station in Options → Audio Setup, starts a new game, plays the intro cutscene
and rides the opening mission's BMX. It captures the mix to a WAV, checks that all eleven station
streams and the cutscene stream open, and fails on silence, a crash or an `[INTERP]` hit. The
11,868-frame run passes with sound from the first menu retune to the end.

What is in place:
- **Mixer** (`host/src/audio/mix.cpp`): every playing secondary buffer is resampled (linear) from
  its frequency to 48 kHz stereo float. 8- and 16-bit PCM, mono and stereo. Volume and pan follow
  DirectSound's hundredths of a decibel, pan attenuating the far side only. The sum is clamped.
- **Play cursors follow the mixer.** A buffer's position advances only as the mixer consumes it,
  so streaming, `GetCurrentPosition` and cutscene timing all run on the audio clock instead of
  wall time. Non-looping buffers stop and rewind at the end. The write cursor leads by 10 ms.
- **DS3D**: buffer and listener parameters, deferred (`DS3D_DEFERRED`) and immediate, with
  `CommitDeferredSettings`. Mono 3D buffers are panned by the source's direction in the listener's
  basis (right = top × front); rolloff uses the DS3D inverse-distance law between min and max
  distance. SA puts the listener at the origin facing +Y with top −Z, sets rolloff and doppler to
  0, attenuates by distance itself through `SetVolume`, and Doppler-shifts with `SetFrequency`.
  DS3D therefore only pans. Velocities and Doppler are stored, not applied.
- **Output** (`host/src/audio/output.mm`): the default-output AudioUnit at 48 kHz.
  - A change of default device reopens the unit. System sleep stops it and wake restarts it.
  - With no device, or with `--no-audio`, a 10 ms system-clock thread drives the same mixer
    silently, so the game's audio clock never stalls.
  - `--audio-capture FILE.wav` records the mix as 16-bit stereo. The WAV is finalized on every
    exit path: `ExitProcess`, `--exit-after-frames`, and `exit()` from Cmd-Q, window close or
    WinMain returning.
- `--ignore-user-input` drops real keyboard and mouse events (not Cmd-Q) so scripted runs cannot
  be disturbed by someone using the Mac.
- **Movie audio**: pl_mpeg decodes the intro movies' MP2 track 0.2 s ahead of the video into a
  FIFO stream (`host/src/audio/stream.cpp`). The output mixes it after DirectSound and resamples
  it to 48 kHz. Pausing the movie detaches the stream. The logo (15.3 s) and title (88.7 s)
  movies play their full soundtracks with no gaps.
- `IDirectSoundBuffer8::SetFX` returns `DSERR_CONTROLUNAVAIL`, as for a buffer created without
  `DSBCAPS_CTRLFX`. SA never calls it.
- `vorbisfile.dll` (M3) and the audio stream thread (a real guest thread) are unchanged.

Findings:
- SA's PC traffic per frame is about 8 `GetCurrentPosition`, 8 `SetVolume`, 7 `GetStatus` and 2
  DS3D `SetPosition` calls, plus one `CommitDeferredSettings`. Each one-shot sound gets a fresh
  3D buffer: `CreateSoundBuffer`, `Lock`, `Unlock`, `SetCurrentPosition`, QI for
  `IDirectSound3DBuffer`, `SetVolume`, `Play`.
- SA asks a buffer for `IKsPropertySet` (EAX) once. It is still refused, so the game runs its
  non-EAX path; accepting and ignoring it would sound the same. Reverb stays an open question (§9).
- A destroyed joinable `std::thread` (the silent-clock thread) aborted the process with a bare
  `libc++abi: terminating` when `exit()` ran static destructors. The output state is now never
  destroyed, and an `atexit` hook stops the thread. `tests/audio/test_audio_exit.cpp` covers it.
- Real mouse movement over the window changes the hovered menu item, which made scripted menu
  runs pick the wrong entry. Routes now pass `--ignore-user-input`.
- The intro mission's Ballas drive-by kills a player who lingers by the street, which made
  gameplay-based radio tests unreliable. The Audio Setup page plays each station as it is
  selected, which gives a deterministic test.

Deviations from the plan:
- Controller support is deferred to M11 (owner's decision). The DInput8 joystick and the
  GameController bridge are implemented and unit-tested, and work with scripted input, but have
  not been tested with real Xbox or DualSense controllers or real hot-plug. See M11.

Not done in M5, carried forward:
- 3D positioning compared with the reference at fixed scenes (M7 harness).
- Lip-sync and station pitch checked by a person. Both follow from the mixer-driven play cursor
  and the resampler, but nobody has watched or listened yet.
- EAX reverb approximation (§9).
- `DuplicateSoundBuffer` copies the samples instead of sharing them. SA never calls it.
- `ClipCursor` and relative mouse capture, so mouse look keeps working when the pointer would
  leave a window. Done in M6, with the window modes.

Work:
- Fake DirectSound8 + EAX on CoreAudio (§3.7). Native libvorbisfile with guest callbacks.
  Audio stream thread.
- Audio device change and sleep/wake handling.

Exit criteria:
- SFX, all radio stations, mission dialogue and cutscene audio play, and cutscene lip-sync
  stays in sync.
- 3D positioning matches the reference at fixed scenes.

### M6 — Playable vanilla (+ launcher MVP)

Goal: the unmodified game is playable end to end through the launcher.

Status: **in progress.** The window modes, mouse capture, saves, frame-time capture and the
launcher MVP are in place. The mission playthroughs and the 2-hour soak still need a person
(see below). Windows save round-trips are blocked on a save made on Windows and on a Windows
machine or the reference to load ours. A clean-Mac run is blocked on bundling a linker.

What is in place:
- **Window modes** (`host/src/platform/window.mm`): `--window-mode windowed|borderless|fullscreen`
  and `--display N`.
  - Windowed sizes the window to the game's resolution in pixels, so 1280×720 on a 2× display is a
    640×360-point window.
  - Borderless covers the whole display, hides the menu bar and Dock, and keeps the notch area
    black.
  - Native full screen uses its own Space. Control-Command-F toggles it.
  - The picture is letterboxed to keep its aspect ratio, and pointer coordinates are mapped
    through the same rectangle, so menus line up in every mode.
  - The game's mode list follows the chosen display's usable pixel size.
- **Mouse capture** replaces `ClipCursor`. While the game hides the pointer and its window is key:
  - the real pointer is frozen and hidden (`CGAssociateMouseAndMouseCursorPosition`), and mouse
    look reads raw motion;
  - a virtual pointer, confined to the picture, drives `WM_MOUSEMOVE` and `SetCursorPos`.
  - Command, Command-Tab, losing focus or the game showing the pointer releases it at the virtual
    position. A click captures it again.
  - `--no-mouse-capture` turns it off, and `--ignore-user-input` runs never capture. SA only ever
    calls `ClipCursor(NULL)`.
- **Mouse look** reads raw `GCMouse` deltas while the window has focus, as DirectInput reports raw
  counts on Windows (Y negated). When no raw motion has arrived in the last 0.25 s, the AppKit
  deltas are used instead, in pixels. Fractional motion carries over between `GetDeviceState`
  reads instead of being truncated. Confirmed in play.
- **x87 condition codes**: `sa-recomp` keeps C0–C3 across `fld`, `fadd` and the other instructions
  that do not set them, as real CPUs do (see Findings). Doors swing open when pushed, and peds
  collide with box-shaped objects again. Confirmed in play.
- **Shadow debugging** (`host/src/shadow.cpp`): `--shadow` runs sampled calls lifted and
  interpreted in forked children on live game state and compares registers, the x87 stack and
  every page written. `--probe`, `--touch`, `--dump-memory`, `--pool-stats` and `--pool-detail`
  help narrow a bug down to one call (README, Debugging).
- **Quit**: Cmd-Q closes the game the way its window's close button does (see Findings).
- **Saves.**
  - SA writes `GTASAsfN.b` into the user dir through the existing Documents mapping.
  - A `saveN` key-script action calls the same save routine as the save menu (`0x619060`), and
    refuses while `OnAMissionFlag` is set.
  - `tools/route/m6_save.py` makes a save after failing the opening mission, then loads it from
    the menu and checks the money.
  - `tools/save/sa_save.py` checks the size (202,752 bytes), the block tag and the checksum.
- **Frame-time capture**: `--frame-times FILE.csv` and a `[PERF]` summary. `tools/perf/frame_report.py`
  prints percentiles and a histogram. `m4_route.py` records `out/route_frames.csv` by default.
- **Launcher MVP** (`launcher/`, SwiftUI, built by CMake as `MacAndreas.app`):
  - game folder selection and `sa-verify`;
  - conversion through the bundled `sa-recomp` with progress;
  - `stamp.json` hashing the exe, analysis, manifest, guest runtime and `sa-recomp`, with
    reconversion when the stamp changes;
  - display, window mode, aspect ratio, resolution and intro choices;
  - launch with logs in `~/Library/Logs/MacAndreas`.
  - The bundle carries its own `libLLVM`/`libzstd` with rewritten install names, so `sa-recomp`
    no longer loads Homebrew.
  - `--headless status|convert|snapshot` drives it without a window.
  - Checked end to end on this Mac with the bundled tools only:
    - conversion into `~/Library/Application Support/MacAndreas/build` took 15 minutes;
    - the stamp then read up to date;
    - the bundled `sa-host` booted the converted library at 2560×1440 borderless.
  - The intermediate shards (about 750 MB) are deleted after a successful conversion, leaving a
    234 MB library.

Findings:
- A save made during a mission crashes on load in the script interpreter: the mission's script
  position is saved, but the mission code is not reloaded. Windows behaves the same, which is why
  the game only offers saving outside missions. The debug save action checks the flag for that
  reason.
- Saving through the save routine moves the game clock forward (20:07 at save time, about 03:07
  in the file). This is game logic, not the port.
- GenericSave's block switch was missing 25 trace entry points (`0x5d14d5`–`0x5d15ab`); they are
  now lift roots.
- The launcher's resolution is only a starting point. Like `--resolution`, it loses to a mode saved
  in `gta_sa.set`.
- Cmd-Q went through AppKit's `terminate:`, which calls `exit()` while the game's threads are still
  running. Static destructors then destroyed a `recursive_mutex` in use, and the process aborted
  (`recursive_mutex lock failed`). Cmd-Q now posts `WM_CLOSE`, as the close button does, and the
  game exits through `ExitProcess`. A second Cmd-Q quits at once. When WinMain returns,
  `exit_process` runs too, never the static destructors.
- `--ignore-user-input` does not cover the title-bar buttons or Cmd-Q, so someone using the Mac
  can still end or resize a scripted run.
- Mouse look barely moved the camera. The host truncated AppKit's fractional, accelerated point
  deltas to integers, and SA turns 0.0025 rad per DirectInput count, so most motion was lost.
- Doors did not open and peds walked through objects. Remill's x87 semantics write
  `__remill_undefined_8()` (0 at run time) to C0, C2 and C3 for `fld`, `fild`, `fadd`, `fsub`,
  `fmul`, `fdiv` and others. Real CPUs leave them unchanged, and MSVC relies on that: it schedules
  `fnstsw` after unrelated loads and arithmetic and branches on an earlier `fcomp`.
  `CCollision::ProcessSphereBox` (`0x4122c1`) does this, so a sphere whose centre lies inside a
  box on one axis was treated as outside and never touched it. CJ passed through the front door
  of his house without moving it. `sa-recomp` now drops those 22 writes, so the codes keep their
  previous value. The interpreter already did, which is how `--shadow` caught it at the door:
  lifted `0x412130` returned false where the interpreter returned a contact. The randomized
  `--diff-test` missed it, because random spheres and boxes almost never overlap.
- First frame-time numbers, from the in-world route on a dev build (host code at `-O0`, lifted code
  at `-O2`) at 1280×720 on an M5: 13,125 in-world frames, mean 41.6 ms (24 fps), p50 41.7, p95
  50.1, p99 94.2, max 213 ms. The menus run at the 120 Hz display rate. The M11 baseline needs a
  release build on the base M1 named in the exit criteria.

- `--shadow` has two known sources of noise. Timer and audio-clock words such as `0xb729a4` and
  `0xb610e0` differ because the interpreted run is slower. Calls that enter a trace in the middle of
  a function have no return address on the stack and end as inconclusive.

Still to do in M6:
- A regression test for the x87 condition codes. The lift smoke test bypasses `sa-recomp`'s
  fixups, and `--diff-test`'s random spheres and boxes almost never overlap, so neither covers it.
- Play the exit-criteria missions through, and run the 2-hour soak, by a person (see Exit
  criteria).
- Windows round trip: load a save made on Windows here, and load a save made here on Windows (or
  in the CrossOver reference).
- Clean Mac: `sa-recomp` still links through `xcrun clang`, which needs the Command Line Tools.
  Bundle `ld64.lld` (from the pinned LLVM) and link with it, then test on a machine without
  Homebrew or Xcode. The analysis data question (§9, analysis is local-only) decides what the
  bundle can ship.
- Capture the M11 performance baseline on the fixed routes.

Work:
- Save/load in the user dir. Saves round-trip with the Windows 1.0 US build in both directions.
- Launcher MVP: locate the game folder, `sa-verify`, run `sa-recomp` with progress, write the
  stamp, choose resolution/aspect/display, launch `sa-host`.
- Window modes: windowed, borderless fullscreen, native fullscreen. Retina, multiple displays.
- `ClipCursor` and relative mouse capture (from M3/M5).
- Baseline performance capture (frame-time histogram on fixed routes) for M11.

Exit criteria:
- These missions are completed end to end: In the Beginning, Ryder, Tagging Up Turf,
  Cleaning the Hood, Drive-Thru, Nines and AK's, Drive-By, Sweet's Girl, Cesar Vialpando.
- Saves round-trip with Windows.
- A 2-hour soak test passes with no crash and zero `[INTERP]` hits.
- A clean Mac with only the exe and game folder goes from launcher to in-game with no other
  installs.

### M7 — Mod infrastructure

Goal: tooling that turns upstream mod source into verified manifest entries.

Status: **complete.** The comparison against a Wine/CrossOver reference moved to M11 (see
"Reference check" there); the ported fixes were checked in scripted runs against the behaviour
expected from the mods on Windows. The inventory, pinning, triage records, the CI conflict check, unified
settings and `call_redirect`/`mid_hook` lowering are in place. Four SilentPatch groups are ported,
one for each manifest kind, and checked in a scripted run, which also captures a camera-pinned view.
The rule-based triage pass is done; fix-by-fix decisions belong to M8–M10.

What is in place:
- **Inventory** (`tools/mod-inventory/mod_inventory.py`, Python stdlib only).
  - `fetch` clones the commits pinned in `mods.json`, the same ones as `THIRD_PARTY_NOTICES.md`, with
    their submodules, into `.cache/mods/`.
  - `scan` reads the files each mod's own project compiles. It strips comments, evaluates `#if` with
    that project's defines, and tracks functions, lambdas and `if` blocks. It records ModUtils calls
    (`Patch`, `Nop`, `InjectHook`, `InterceptCall`, `ReadCall`, the displacement helpers), `injector::*`,
    `safetyhook::create_mid/inline`, `AddressByVersion`/`External*` bindings and literal data reads.
  - Each row has a stable id, op, 1.0 US address, span, how it was pinned, version, fix group
    (the nearest heading comment), scope, `if` condition, target and source line.
  - The output is `mods/<mod>/inventory.yaml`. Pattern text is redacted, so rows hold addresses only.
- **Pinning.**
  - SilentPatch's version-specific functions decide the version (`*_10` keeps; `*_11`, `*_Steam` and
    `*_NewBinaries*` drop). `AddressByRegion_10` and `AddressByVersion` take the 1.0 US address.
  - Pattern scans (`hook::pattern`, `get_pattern`, `get_first`, `count(n).get(i)`, `PatternAndOffset`)
    are resolved against the local exe's code range, or against an explicit `{{ lo, hi }}` range in any
    section. `GetFunctionAddrIfRerouted` follows a leading `jmp` within the image, as SilentPatch does,
    which matters for functions the DRM moved into `.HOODLUM`. `GetBranchDestination` and pointer reads are
    resolved from the exe as well.
  - Loop counters expand to every match. Range-for address lists and helper parameters (through the
    helper's callers in the same file) are followed.
  - Anything left over is `pin: unresolved` with a reason. `mods/<mod>/pins.yaml` pins it by hand,
    with one `addr` or a list of `addrs` (a helper called for several sites becomes `<id>.0`, `<id>.1`, …).
- **Triage** (`mods/<mod>/triage.yaml`): one record per fix group with `decision`
  (pending/keep/replace/drop), `reason` and `changelog` (`CHANGELOG-SA.md:<line>`, matched by words
  for SilentPatch). Keyword hints suggest replace/drop for FLAC, INI, frame limiter, D3D9/DInput/DSound,
  display-mode/window-message and other-mod compatibility groups. Decisions survive a rescan.
- **Checks.**
  - `manifest-check DIR...` (C++, on the manifest library) runs the strict checks that need no game
    data. The library gained two: a group must not mix sources or settings, and a `setting` must be a
    known key (through `CheckContext::is_known_setting`, for when unified settings land).
  - `mod_inventory.py check --manifest manifest` validates inventory and triage and lists cross-mod
    overlaps. It requires every mod entry in the manifest to port a site whose group is `keep`.
  - CI runs both. `tests/manifest/seeded/overlap.yaml` is a seeded overlap that must fail (CTest
    `manifest.check_rejects_seeded_overlap` and `manifest.seeded_overlap_reported`, plus a CI step).

- **Unified settings** (`runtime/settings`, `MacAndreas::settings`).
  - `mods/settings.yaml` is the schema: key (`<mod>.<name>`), `bool`/`int`/`number`/`choice`, default, title,
    notes, and the INI entry it `replaces`.
  - The launcher bundles it, shows the settings in a Fixes section and writes
    `~/Library/Application Support/MacAndreas/mods.conf` (`key = value`). `sa-host` reads it with
    `--settings`, takes `--set KEY=VALUE` overrides, and natives read values through
    `mod_settings()`.
  - `manifest-check --schema` and `sa-host` reject a `setting` that is not in the schema. An entry's
    `setting` must be a bool; when it is false at launch the entry is not registered.
  - `byte_patch` and `nop` can't take a `setting`: they are lifted in, so switching them would mean
    lifting both variants. That is left until a group needs it.
- **Lowering in `sa-recomp`, with matching `sa-host` runtime.**
  - `call_redirect`: the `call rel32` or `call [mem]` at the site is rewritten, both in the lifted
    image and in guest memory, to call `0xE0000000 + site`. The host registers that address to a
    trampoline. It calls the native target, which can read `redirect_original()`, or jumps to the
    original callee when the entry's setting is off.
  - `mid_hook`: the address becomes a trace head. Its lifted trace is renamed `sub_<addr>_body` and
    wrapped. The wrapper calls `__ma_mid_hook`, which runs the native hook on the guest state. The
    hook returns where to resume: the site to continue, or another address, reached through
    `__remill_jump`. Fallthrough, jumps and the interpreter all enter through the wrapper.
  - Host runtime tests cover the trampoline (on, off, `redirect_original`) and mid-hook resume.
    `recomp.mid_hook_wrapper` builds small modules and checks that `wrap_mid_hook` sends same-module
    callers through the wrapper, and that `route_mid_hook_bodies` repairs older shards.
- **First SilentPatch ports** (`manifest/silentpatch.yaml`, natives in
  `host/src/native/silentpatch.cpp`; their groups are triaged `keep`):
  - `byte_patch` *Fixed ammo for melee weapons in cheats*. SilentPatch patches the immediate inside
    `push 0`, which fails the instruction-boundary check, so the entries rewrite whole instructions
    (`push 1`, and the katana's two pushes swapped).
  - `func_replace` *Locale based metric/imperial system* (`0x56d220`, `sp_is_metric`):
    `kCFLocaleUsesMetricSystem`, or `silentpatch.units` = `metric`/`imperial`, which replaces the
    SilentPatch INI's `Units`.
  - `mid_hook` *Jetpack hover with keyboard and mouse* (`0x67ed2d`): when look-behind is pressed it
    resumes at the hover path (`0x67edaf`). `silentpatch.jetpack_keyboard_hover` switches it.
  - `call_redirect` *Display a fallback string if the resolution string is absent* (`0x57a071`,
    `sp_ascii_to_gxt_null_check`): Advanced Display Options passes the mode string through the native,
    which substitutes `"-"` for a null pointer and calls the original `AsciiToGxtChar`. The native
    mode list makes the null case unlikely here, so this is a safety net and the `call_redirect` proof.
- **Harness pieces.**
  - `--watch-chain ADDR>OFF>…` follows pointers, for player and vehicle state.
  - `tools/verify/image_diff.py` compares a capture against a reference with a per-channel tolerance,
    an allowed fraction, ignored rectangles and a magenta diff image.
  - **Camera pinning.** `sa-host --pin-camera FRAME:X,Y,Z:TX,TY,TZ` calls
    `CCamera::SetCamPositionForFixedMode` (`0x50bec0`) and `CCamera::TakeControlNoEntity` (`0x50c8b0`,
    jump cut, script control) on `TheCamera` (`0xb6f028`) from the guest thread at that frame;
    `FRAME:restore` calls `CCamera::Restore` (`0x50b930`). These are the functions behind the script
    commands `SET_FIXED_CAMERA_POSITION`, `POINT_CAMERA_AT_POINT` and `RESTORE_CAMERA`: the handlers
    at `0x47c540` and `0x47c598` load `ecx` with `TheCamera` and pass the script parameters
    (`0xa43c78`) to them. So a CLEO script on the reference can produce the same view.
  - `tools/route/m7_silentpatch.py` runs the ports: a main-menu run into Advanced Display Options,
    then the New Game route. The route pins the camera on each view in `VIEWS`, captures it and
    restores the camera; `--reference DIR` diffs those captures against another run's (clock ignored). `Route.new_game()` clicks through the intro cutscene, so CJ is in control
    at frame 1800 instead of 6450.

Scripted results on this Mac (`tools/route/m7_silentpatch.py`, converted with the manifest):
- **Melee cheat ammo:** after the second weapon cheat, the melee slot holds the knife with total
  ammo 1 (vanilla gives 0).
- **Jetpack hover:** with the jetpack cheat and Left Shift (thrust) held, holding Q+E (look behind)
  freezes the height at about 56.6, as hovering does. With `--set
  silentpatch.jetpack_keyboard_hover=false` the hook is not registered and CJ climbs about 100 units
  through the same window, as in vanilla.
- **Units:** the game calls `CLocalisation::Metric` on the Misc stats page. The native answers
  imperial for this Mac's US region by default, and metric with `--set silentpatch.units=metric`.
- **Pinned view:** the camera pins behind CJ at the first-control spot and restores 40 frames later.
  Two runs of the route match within 0.005–0.17% of pixels (distant traffic, foliage); the threshold
  is 0.5%.
- **Resolution string:** in Advanced Display Options the call at `0x57a071` reaches the redirect with
  `"1280 X 720 X 32"`, returns through the original `AsciiToGxtChar`, and the row draws unchanged.

None of this has been compared against a Wine/CrossOver reference; that check is in M11.

Inventory after hand pins and the rule-based triage pass (sites per decision):

| Mod | Sites | Unresolved | Fix groups | Keep | Replace | Drop | Pending | Other versions dropped |
|---|---|---|---|---|---|---|---|---|
| SilentPatch | 661 | 0 | 207 | 12 | 61 | 15 | 573 | 707 (1.01: 178, Steam: 215, newsteam: 314) |
| Widescreen Fix | 325 | 32 | 84 | 0 | 9 | 2 | 314 | none (pattern-based) |
| Project2DFX | 52 | 0 | 12 | 0 | 0 | 0 | 52 | none (pattern-based) |

The rule-based pass follows §5 step 3: Windows display-mode, window-message, DirectInput, frame
limiter, file-flag and semaphore fixes are `replace` (the native layer already owns them), and ASI,
INI, FLAC, DirectPlay, single-instance and CLEO code is `drop`. Game logic, HUD and renderer groups
stay `pending` for the milestone that ports them (M8, M9, M10). Bindings groups (`bindings: <file>`)
are reads of game functions, not patches, and are decided with the groups that use them.

Findings:
- At the pinned commit, the SA Widescreen Fix is `source/GTASA.FusionFix` in WidescreenFixesPack. It
  covers more than widescreen, so its triage needs to separate the widescreen groups from the rest.
- SilentPatch and the Widescreen Fix both patch the same sites:
  - HUD scaling at `0x58c387`–`0x58c4e4`;
  - the video-mode list and resolution dialog at `0x74596c`–`0x746368`, next to the native
    `GetVideoModeList` replacement (`0x745af0`), which now owns that code;
  - the frame limiter call at `0x748d9b`.

  Triage has to pick one owner for each.
- The unresolved Widescreen sites come from three things:
  - lookups kept in a struct (`MenuConstraint`'s `Locate()`);
  - subscripted address tables (`m_dwHUDWidth[i]`);
  - six patterns that do not occur in the 1.0 US exe (`EntryExit.ixx`, `Frontend.ixx:119`, `Draw.ixx:114`).

  The Widescreen ones are left for M8, which ports that code. Eight of the nine SilentPatch ones are
  hand pinned: vtable slots read through an instruction operand (`CVehicleModelInfo` `0x85c5c8`,
  `CAutomobile` `0x871120`, `CPlane` `0x871948`, `CTrailer` `0x871c28`), the cheat jump table
  (`0x8a5cc8`) and the six rotor/propeller render sites.
- `0x41bfa0` (*Cancel the Drive By task of biker cops*) and `0x45c4b0` (*PlayerPedDataAssignment*)
  are DRM thunks: each is a `jmp` into `.HOODLUM` (`0x15612e0`, `0x1561cf0`). SilentPatch follows
  the jump through `GetFunctionAddrIfRerouted`, but the scanner treated that as identity. It put the
  five *PlayerPedDataAssignment* rows at `0x45c50d` (in the thunk's padding) instead of `0x1561d4d`,
  and couldn't find the biker-cop pattern. Both now resolve (`0x1561366` for the biker cops).
- The first keyword hints were wrong for three groups. `InjectDelayedPatches_10` (22 sites of 1.0
  fixes) was hinted "other versions", and credits and dialog text scaling were hinted "window and
  mode handling" because they mention resolution or windows. The other-versions hint is gone (the
  version rules already drop those rows) and the display hint now needs specific terms; a unit test
  covers these cases.
- The first `mid_hook` build skipped the hook. Renaming the lifted trace to `sub_<addr>_body` left
  calls in the same shard pointing at the renamed function, so callers bypassed the wrapper.
  `sa-recomp` now redirects those uses at lift time. The compile step does the same
  (`route_mid_hook_bodies`), so shards lifted before the fix are corrected by `recompile`.
- `--probe` reported no calls to the jetpack control function (`0x67eb90`) even while it ran. Its
  only caller is a `jmp` thunk at `0x4075eb`, which lifts to a direct tail call and never goes
  through the function table that the probe hooks.
- Vehicle look-behind on a keyboard is Q and E together, which is what `GetLookBehindForCar` reads.

Work:
- `tools/mod-inventory`: scans each pinned upstream commit for `InjectHook`, `Patch`, `Nop`,
  `ReadCall`, `MakeInline`, `injector::*`, pattern scans and memory writes. Output is one YAML
  row per site.
- Pinning: resolve version branches and pattern scans to concrete 1.0 US addresses.
  Newsteam/RGL branches are dropped automatically.
- Triage records (keep / replace natively / drop + reason), grouped by fix and linked to
  upstream changelog entries.
- Strict conflict checks (§3.10) in CI.
- Unified settings: one launcher-owned file replaces every mod INI. Mod code reads settings
  through one API.
- Verification harness: fixed saves, input playback, camera pinning, screenshot capture and
  diff against the Wine/CrossOver reference.

Exit criteria:
- Inventory YAML exists for all three mods at pinned upstream commits.
- The conflict check runs in CI and fails on a seeded overlap.
- One `byte_patch`, one `mid_hook` and one `func_replace` taken from SilentPatch are each
  verified in a scripted run against the behaviour expected from the mod on Windows. Met, with a
  `call_redirect` as well. The comparison against a Wine/CrossOver reference moved to M11.

### M8 — Widescreen Fix

Goal: correct presentation at any aspect ratio.

Work:
- Keep: HUD, radar, menu and text scaling and positioning, FOV/aspect math, crosshair,
  cutscene and letterbox handling.
- Replace natively: resolution and mode enumeration, window sizing.
- Drop: INI parsing, code for other games.

Exit criteria:
- HUD, radar, menus, crosshair, FOV and cutscenes are correct at 4:3, 16:9, 16:10 and 21:9 on fixed
  saves and cameras, checked in scripted runs against the Widescreen Fix's behaviour on Windows. The
  comparison against a Wine/CrossOver reference is part of M11's reference check.

Status: **in progress.** The base layer is ported: the real aspect ratio, a Hor+ field of view and
the sky spread that follows it (`manifest/widescreen.yaml`, natives in
`host/src/native/widescreen.cpp`, switched by `widescreen.aspect_fov`). The HUD, text and menu
scale layer follows (`manifest/widescreen-hud.yaml`, switched by `widescreen.hud`), with script text,
menu and HUD positions and the 16:9 menu canvas. Cutscene borders, the cutscene FOV and the wheel
camera FOV are in place, and so are splash screens, menu sliders, decimal settings, the full-screen
map, the width-640 shortcuts and the water scan for the wider view. Every widescreen group of the fix
is decided; the FusionFix features outside M8's scope and a HUD misalignment reported at 2560×1440 move
to M11.

What is in place:
- **Aspect ratio and Hor+ FOV** (group *Aspect ratio and Hor+ field of view*, regrouped in
  `mods/widescreen/pins.yaml` from `Draw.ixx` and `Main.ixx`'s `CameraSize` hook):
  - `func_replace` `CDraw::CalculateAspectRatio` (`0x6ff420`): `RsGlobal` width/height
    (`0xc17044`/`0xc17048`) into `ms_fAspectRatio` (`0xc3efa4`), instead of 4:3, 16:9 from the
    Widescreen menu option or 5:4 under cutscene borders.
  - `func_replace` `CDraw::SetFOV` (`0x6ff410`): stores `ms_fFOV` (`0x8d5038`) and a Hor+ copy (the
    4:3 vertical angle widened to the screen) outside cutscenes and borders
    (`CCutsceneMgr::ms_running` `0xb5f851`, `TheCamera.m_bWideScreenOn` `0xb6f065`).
  - `mid_hook`s: `CameraSize` (`0x72fc70`) gets the Hor+ view window and the real aspect ratio;
    `CalculateFrustumPlanes` (`0x514d63`) loads the Hor+ half angle; `Find3rdPersonCamTargetVector`
    (`0x5149a6`) scales the crosshair offset by the aspect ratio; the two aim-width multiplies
    (`0x50ad59`, `0x51498d`) use the Widescreen Fix's 0.01403292 and the `fdiv` by the aspect ratio
    (`0x50ad79`) is skipped.
- **Sky spread** (*Fix sky multitude*): the two multiplies by 1.4 (`0x714841`, `0x71485e`) become
  1.4 × aspect/(4:3).
- **Sprite size** (*Sprite::Sprite*): both `CSprite::CalcScreenCoors` overloads (`0x70ce30`,
  `0x71da00`) size a sprite's width from the screen width and its height from the screen height, so
  coronas, the sun's glow and similar sprites stretch with the window. The Widescreen Fix uses the
  height for both. The port pushes the screen height at the width's `fild` (`0x70ced8`, `0x71da6d`).
  In cutscenes, the four 70/FOV multiplies (`0x70cef6`, `0x70cf08`, `0x71da8b`, `0x71da9d`) are
  operand slots scaled by the cutscene sprite scale (aspect/(4/3)/zoom), as the mod's `CalcScreenCoors`
  does.
- **`site:`.** A manifest entry can name the inventory row it ports when it hooks a different
  instruction than the mod does (the Widescreen Fix replaces `CalcScreenCoors` at its entry; the port
  changes one instruction inside). `mod_inventory.py check` then checks that row's triage instead of
  an address overlap.
- The Widescreen Fix writes a pointer to its own float into an instruction operand at several of
  these sites. The port does the arithmetic in a `mid_hook` and resumes after the instruction
  instead, so no guest-side constant is needed and every entry can take the setting.
- **`mid_hook` resume addresses.** A `mid_hook` entry can list `resume:` addresses. `sa-recomp`
  makes them trace heads, so resuming there stays in lifted code. Before this, the jetpack hook's
  resume (`0x67edaf`) ran through the interpreter.
- **Scanner.** Pattern scans now cover the image from its base to the end of the last executable
  section, as Hooking.Patterns does. That includes the DRM-moved code in `.HOODLUM`, where
  `CDraw::SetFOV`'s caller and the `EntryExit` patterns are (Widescreen unresolved: 32 → 28).
- **FOV factor.** `widescreen.fov_factor` (0.5–2.0, default 1.0; the mod's `FOVFactor`) multiplies
  the tangent of the gameplay half angle in `ws_set_fov`, capped at a 150° view as in the mod.
  Cutscenes keep their field of view.
- **HUD, text and menu scaling** (group *HUD, text and menu scaling*, regrouped from `Frontend.ixx`).
  The mod points each scaled instruction's operand at its own float. The port does the same with two
  new manifest kinds (§3.10): `operand_slot` (the lifter points the operand at a per-instruction slot
  that the host fills each frame) and `data_slot` (a float in the game's data set by a native).
  - 107 scale factors after `fild RsGlobal.maximumWidth` keep their startup value divided by
    aspect/(4/3) (`ws_hud_scale`; the mod's `vHudScalePtrs`, from the same three patterns, its four
    extra sites and without its excluded sliders, map edges and message-screen positions).
  - The mod's HUD element lists: 92 widths become (1/640)/(aspect/(4/3)) and 82 heights 1/448, each
    times `widescreen.hud_width`/`widescreen.hud_height` (0.5–1.5, default 0.8 as in the mod); the
    weapon icon and ammo widths 0.17343046/(aspect/(4/3)); the weapon icon's x 47 instead of 58.
  - The shared 1/640 (`0x859520`) narrows for its remaining users, and the shared 640 (`0x85951c`)
    becomes 480 × aspect; the main menu, its entry sprite and the briefs text keep 1/640.
  - Crosshairs use 1/480 for their height. One listed site (`0x58e2c8`) is an `fadd` of 1.0 on 1.0 US;
    the mod points it at 1/480 too, and the port does the same. Rocket launcher and Hydra lock-ons use
    20 instead of 28, so they are circles.
  - Radar `widescreen.radar_width` × `widescreen.radar_height` (85.5 × 78 by default, as in the mod),
    with `CHud::DrawPlayerInfo`'s bars kept at x 94; gameplay subtitles at 1.2 × 0.58 times
    `widescreen.subtitles_scale`, centered between the radar's margins, and moved up when they would
    reach the bottom edge (`0x58c68a`); other languages keep the English letter width.
  - The menu helper text (`StretchX(610)` at `0x57e3a5`) is anchored to the right edge.
  - The co-op in-car crosshair is a circle: the mod's byte patch at `0x743bcd` points an `fmul` at
    the width argument. The port does that multiply in a `mid_hook` at `0x743bca`, so it follows
    `widescreen.hud`.
- **Decimal settings.** The settings schema has a `number` type (`min`, `max`, `step`), read with
  `Values::number` and shown by the launcher as a stepper. The Widescreen Fix's settings keep the mod's
  units: `widescreen.fov_factor`, `hud_width`, `hud_height`, `radar_width`, `radar_height` and
  `subtitles_scale`.
- **Cutscene borders** (group *Cutscene borders*, from `Sprite2d.ixx`):
  - `func_replace` `CCamera::DrawBordersForWideScreen` (`0x514860`). It computes bars that keep the
    vanilla cutscene window's framing: pillarbox on screens wider than (4/3)/0.7167 ≈ 1.86, letterbox
    otherwise, with the mod's center fudge and minimum top bar. The bars slide in and out over 0.35 s,
    and sudden fades clear them.
  - The zoom that keeps that framing feeds `ws_set_fov`'s cutscene branch (2·atan(tan(FOV/2)·zoom))
    and the sprite scale. The camera's motion blur alpha is set to 80 as in vanilla.
  - Settings: `widescreen.cutscene_borders` (off, letterbox, pillarbox, both; default both) and
    `widescreen.cutscene_border_animation` (default on). The mod keeps the mode in the menu's
    Widescreen row and relabels that row; the port takes it from the launcher, so the in-game row has
    no effect while the fix is on. The vanilla row only chose a 16:9 aspect ratio, which the real
    aspect ratio replaces.
  - Two `mid_hook`s skip the conditions around the two calls (`0x53e2a4`, `0x5af8b9`), so the
    function runs every frame and can animate the bars out. Vanilla only calls it while the camera is
    in widescreen and the menu's Widescreen option is off.
  - A `mid_hook` at `CHud::DrawHelpText` (`0x58bb88`) moves the help text down by the visible top bar
    instead of vanilla's fixed 56.
  - `CFont::PrintChar`'s left cull edge (`0x718a41`) becomes the 4:3 inset, so text moved into the
    centered canvas is not cut.
- **Splash and loading screens** (group *Splash and loading screens*, from `Sprite2d.ixx`). A
  `mid_hook` at `CSprite2d::Draw` (`0x728350`) catches full-screen textured sprites (not the map).
  It points the rectangle argument at a copy that is centered at the texture's aspect ratio (4:3 for
  square and 2:1 rasters), so the caller's rectangle stays as it was, as the mod requires for splash
  fades. It then paints black bars beside it at the sprite's alpha. `tvcorn` and `backbet` get black
  4:3 pillar bars. A `mid_hook` on the loading screen's `DrawBarChart` call (`0x590480`) moves the
  bar into the centered splash, or under the logo of widescreen LOADSCS art.
- **Menu sliders** (groups *Menu sliders* and *Slider rendering…*, from `Frontend.ixx`). The five
  `DisplaySlider` calls draw the slider 140 scaled units from the right edge, 100 wide with 3-unit
  notches, returning at least its left edge for mouse hit tests. The ten hit-test calls to `StretchX`
  return the slider's left edge and the screen's right edge.
- **Full-screen map** (`manifest/widescreen-map.yaml`, from `MenuMap.ixx`;
  `widescreen.fullscreen_map`, default on as in the mod's `MISC.FullscreenMap`):
  - The map works in a virtual canvas 480 × aspect units wide: the screen's aspect for the full-screen
    map, the menu canvas's for the framed one. 49 operand slots replace the map's constants with that
    canvas's center, inset (0 full-screen, 60 framed), right and bottom edges, cursor limits and edge
    fractions; the origin moves with the center when it changes. Zoom and pan speed is 14 instead of
    7, as on the mod's map.
  - `mid_hook`s: the map opens centered on the canvas; zoom keeps the world point under the cursor in
    place (through the game's own map projection, `0x583530`); the origin clamp shrinks the pan range
    with zoom and runs before tile streaming and drawing too; the zoomed-out overview widens its
    square texture, frame, unrevealed zones and blips by 480/448; the area name moves up 40 units on
    the full-screen map; the legend window and its entries are centered.
  - The seven hit-test calls use the drawn map's edges and center. The full-screen map skips the
    frame's eight `DrawRect` calls (setting-gated `call_redirect`s that return at once).
  - The full-screen map is drawn outside the menu canvas. In the framed map at 21:9, the frame's black
    masks are full-height quads drawn in canvas space; as in the mod's menu canvas, the D3D9 layer
    widens black full-height quads to the physical screen edges while the canvas is active (in both
    the vertex-buffer and UP draw paths), so map tiles don't show in the bars. That also replaced the
    bar painting the menu canvas used before.
- **Width-640 shortcuts** (groups *Remove the width==640 shortcut…*, *Dialog text/window…* and
  *Width alone is not enough…*, from `Frontend.ixx` and `MenuMap.ixx`). Vanilla skips its scaling
  when the window is exactly 640 wide, which is only right at 640×480. The mod points 32 compares at
  -1, so the scale branch always runs:
  - `CMenuManager::StretchX` (`0x5733e0`, a DRM-moved stub) becomes a `func_replace`: x × width ×
    the shared 1/640, without the shortcut.
  - 32 compares (`cmp eax/reg/[RsGlobal.maximumWidth], 640` and two `mov esi, 640`) get one
    `mid_hook`, `ws_width_640`. It decodes the instruction, sets the flags as a compare against a
    width that never matches and resumes after it, so the shortcut follows `widescreen.hud`.
  - The radio scan dialog's track and moving segment (two `DrawRect` calls, `0x57bd70`, `0x57be2b`)
    move into the dialog's centered 4:3 canvas.
  - **`resume: +N`.** A `mid_hook` entry with several addresses can name its resume as an offset from
    each one (`+5`, `+6`, `+10`), so the 32 sites are seven entries.
- **Subtitle width with a narrower HUD** (*These calls belong to gameplay's normal and vital-stats
  branches*): the two `CFont::SetCentreSize` calls in `CHud::DrawSubtitles` (`0x58c603`, `0x58c536`)
  move both margins inward by the HUD offset when `widescreen.hud_aspect` is narrower than the screen
  (0.8 of it on the vital-stats branch).
- **Water at the edges of the wider view** (*InitWaterFrustum*, from `Main.ixx`): a `call_redirect`
  on the water renderer's scan of the camera footprint (`0x6e6e8c` → `0x72cae0`) draws every water
  cell the footprint's convex hull touches, even at a corner, as the mod does. Other shapes and
  out-of-range footprints go to the game's scan. It follows `widescreen.aspect_fov`.
- **Wheel camera** (*CCam::Process_WheelCam*): its fixed 70° FOV (`0x512122`) becomes the inverse of
  the Hor+ conversion, so the view matches vanilla once widened.
- **2D offset in the D3D9 layer.** The mod moves 2D by hooking every `CSprite2d` vertex setup and
  the RwIm2D render functions. The port adds a host-wide x offset to pretransformed vertices in the
  fixed-function vertex shader (`depth_range.z`, set by `d3d9::set_offset_2d_x`). Natives set it
  around the calls that draw a moved element and flush `CFont`'s buffered text on both sides, as the
  mod does. Everything drawn in that call moves, so the mod's separate weapon-icon and `DrawRect`
  hooks are not needed.
- **Script text, menus and HUD positions** (group *Script text, menus and HUD positions*, from
  `Hud.ixx`). The hooked functions' epilogues are DRM-moved, so the port wraps their call sites:
  `call_redirect` for calls, and a `mid_hook` that makes the call and returns to the caller for tail
  jumps.
  - `CHud::DrawScriptText` (`0x58c080`, both calls): script texts and rectangles move by the 4:3
    pillar width before drawing and are restored afterwards; the left and right panels of the mod's
    listed minigames (pool, lowrider, Wheel of Fortune, triathlon, races and so on) go to the HUD edges,
    blackjack and roulette windows by their side, and pool and track-betting labels shrink to fit.
    Which script made each record comes from `mid_hook`s on the nine opcode handlers the mod hooks
    (records added since the last command belong to the script that ran it).
  - Script menus (`0x580e00`, `0x5816e0`; six calls and two tail jumps): the panel's x moves the
    same way, and not on the map.
  - The menu's stats, briefs and radio list pages draw moved right by the 4:3 pillar width.
  - The HUD element calls (player info, radar, vital stats, help text, area and vehicle names,
    timers, trip skip) move toward the center to fit `widescreen.hud_aspect` (`auto`, 4:3, 16:9,
    21:9, 32:9; the mod's `HudAspectRatioConstraint`). Under `auto` nothing moves and the calls go
    straight through.
- **Menu canvas** (group *Menu canvas*, from `MenuConstraint.ixx`). On screens wider than
  `widescreen.menu_aspect` (default 16:9, the mod's `MenuAspectRatioConstraint`), the menu is drawn
  in a centered canvas of that shape. Around `CMenuManager::DrawFrontEnd` (`0x57c290`) and
  `ProcessInput` (`0x57b440`) the port narrows `RsGlobal`'s width to the canvas, refreshes the operand
  slots for it, moves 2D by the inset and moves the mouse fields into canvas space; a `mid_hook`
  after the native mouse clamp (`0x580044`) lets the cursor reach the physical edges. The video mode
  change and `CentreMousePointer` see the physical screen. The mod keeps the bars black by widening
  the menu's black full-height quads; the port paints them with `CSprite2d::DrawRect` before the menu
  draws.
- `tools/route/m8_widescreen.py` plays New Game at 960×720, 1152×720, 1280×720 and 1680×720, pins the
  camera on the same view and checks:
  - the reported aspect ratio, and that the middle of each wider picture matches the 4:3 one (Hor+);
  - that the health bar and the money are the same size in pixels at every ratio, at the same
    distance from the right edge (plus the inset that `widescreen.hud_aspect` asks for);
  - that at 1680×720 the Display Setup menu leaves 200-px black bars beside a 16:9 canvas;
  - the cutscene bars at 4:3, 16:9 and 21:9 in the intro cutscene (frame 3000), against the mod's
    geometry.
  - that the New Game loading screen (frame 1240) is pillarboxed to 4:3 at every ratio;
  - that at 1680×720 the pause menu's map fills the screen, or with `widescreen.fullscreen_map=false`
    sits framed in the 16:9 canvas with black bars, and that a held cursor key pans it.

  `--set widescreen.aspect_fov=false` expects the vanilla stretch instead, and
  `--set widescreen.hud=false` expects HUD widths that grow with the window.

Scripted results (`tools/route/m8_widescreen.py`, the camera pinned behind CJ at first control):
- The game reports 1.3333, 1.6000, 1.7778 and 2.3333 for the four windows.
- The middle 960 columns of the 16:10, 16:9 and 21:9 pictures match the 4:3 picture within 0.39–0.42%
  of pixels (compared scene 560×410, HUD left out; threshold 1%). CJ, the walls and the bike are the
  same size at every ratio, and the wider windows show more at the sides.
- With `widescreen.aspect_fov=false` the same comparison differs by 44–61%: vanilla renders 4:3 and
  stretches it.
- Before the sprite port the comparison differed by 2–4%, all in the sun's glow (a corona sprite
  that widened with the window).
- With the HUD layer the middles match within 0.02–0.03%. The health bar is 68 px wide and the money
  127 px at all four ratios (80% of vanilla's 86 and 160 at 4:3). With `widescreen.hud=false` they
  grow with the window (86/104/115/151 and 160/192/213/279 px), as in vanilla. At 21:9 the radar is
  round and the subtitles are centered.
- Menus at 16:9 and 21:9 keep 4:3 letter proportions, and centered items stay centered. At 21:9 the
  menu sits in a 16:9 canvas with black bars (100% of the side bars dark; before the canvas, the
  background art and helper text reached into them). Menu navigation works inside the canvas.
- The stats page's panel sits 160 px further right at 16:9 than at 4:3, the 4:3 pillar width.
- With `widescreen.hud_aspect=16:9` the 21:9 HUD moves 200 px inward on both sides (health bar and
  money 242/241 px from the right edge instead of 42/41); 4:3, 16:10 and 16:9 are unchanged.
- The scene-middle comparison now differs by 0.11–0.13% between runs, in the animated marker cone,
  the bike and leaves (animation phase), not in the layout.
- Intro cutscene bars (expected from the mod's formula): 4:3 letterbox 75/129 px (75/129), 16:9
  letterbox 17/81 px (16/81), 21:9 pillarbox 171/170 px (170). The picture between the bars shows the
  same shot at each ratio. The wheel camera is not reached by a route yet.
- Loading screen side bars 0/96/160/360 px at 4:3/16:10/16:9/21:9, as expected for its 4:3 art; the
  loading bar sits under the logo (x 235, 270 px wide at 16:9, the mod's 50/640 and 180/640 of the
  splash). The 21:9 Display Setup slider spans x 1270–1420, right-aligned with the value column (it
  was 1200–1395 before).
- Map at 1680×720: the full-screen map fills the screen (10% of the side 200 px dark, all map
  colours) and the framed one sits in the 16:9 canvas with black bars (284/238 dark columns against
  200-px bars, the frame's masks included). Holding a cursor key moves the map cursor to its limit and
  then pans the map at 14 units a frame (34 and 72 frames of origin movement in the route). The legend
  is centered with its two columns at x 600 and 885 (240 and 525 plus the 360 offset); before the
  lifter fix the second column drew at 537, without the offset.
- Width-640 shortcuts: the host accepts heights of 480 and up, so the 640-wide modes it can have that
  are not 640×480 are taller than 4:3. The route compares the Display Setup page at 640×512 with the
  page at 1280×1024 (both 5:4): its white text spans x 279–604 against 558–1208, exactly half. With
  only the new hooks switched off (a scratch manifest), the 640×512 text starts at x 300 and the check
  fails. With `widescreen.hud=false` both sizes stretch alike, as in vanilla, so the check runs only
  with the HUD layer on.
- Water at 3200×900 (32:9, camera pinned low over the sea at Santa Maria pier): with the game's own
  scan, a water cell next to the camera at the left edge is not drawn, and the seabed shows under the
  pier behind a hard line. With the port's scan the water is continuous. A view over the bay is
  identical both ways. The scan tests about 20 cells and calls back for about 15 per frame.
- With the width-640 hooks, the subtitle widths, the co-op crosshair and the water scan, M5's, M7's
  and M8's routes pass, and M4's passes with no interpreter entries (24,450 frames, mean 42.5 ms, p99
  79.2 ms). The slower frames are all in the flying and day-and-night stretch (frames 8,000–16,000).
  That stretch is not repeatable: game time follows real time, so runs fly the jetpack to different
  places (09:29 among sidewalk boxes in one run, 09:06 against a dark building in another). A run with
  only the water scan off measured 39.8 ms, but its jetpack flew elsewhere. A 3-minute run over the
  same inputs with the scan on measured 40–43 ms per 1,000 frames in that stretch.
- With the map and the lifter fix, M5's, M7's and M8's routes pass, and M4's passes with no
  interpreter entries (24,450 frames, mean 38.6 ms, p99 52.4 ms).
- With splash screens, sliders and decimal settings, M5's, M7's and M8's routes pass, and M4's passes
  with no interpreter entries (24,450 frames, mean 38.8 ms, p99 53.7 ms). One earlier M4 run was ended
  by an outside SIGTERM at frame 7,273 with nothing wrong in its log; the rerun passed.
- With the cutscene borders, M5's route (intro cutscene with its audio stream) and M7's pass, and
  M4's passes with no interpreter entries (24,450 frames, mean 39.2 ms, p99 58.8 ms).
- With the position hooks and the menu canvas, M7's route passes and M4's passes with no interpreter
  entries (24,450 frames, mean 40.8 ms, p99 62.1 ms).
- With the HUD layer (326 operand slots, 4 data slots), M7's route passes and M4's passes with no
  interpreter entries (24,450 frames, mean 39.2 ms, p99 56.7 ms).
- M7's route still passes, and the jetpack's resume no longer enters the interpreter.

Findings:
- **`mid_hook`s in loops ran only once.** Remill splits a trace at a known head only when the head is
  not the trace's own start. A loop that jumps back to a `mid_hook`'s own address (the map legend's
  entry loop, `0x5761e0` → `0x5761eb`) branched to the hooked trace's first block and skipped the
  hook after the first pass, so the legend's second column lost its offset. `wrap_mid_hook` now
  redirects such back-edges in the body to a tail call of the wrapper, carrying the loop's memory
  value, as remill does for jumps into other heads (`recomp.mid_hook_wrapper` covers it).
- **A mid-function hook that calls guest code must keep the guest's flags.** The map's zoom-reference
  hook sits on a `jge` and calls the game's map projection, which changed the flags that `jge` tests,
  so map panning stopped. The hook now restores the whole guest state after the call; the mod's
  trampolines save the flags the same way.
- **Lift-time inlining broke call routing.** Remill's per-shard optimizer inlined small traces into
  their callers before the compile step routes guest calls through the dispatcher, so an inlined
  call lost the dispatcher's return-address check, and a trace's tail jump into another trace head
  became an ordinary call. Routing then sent that jump through `__remill_function_call`, which read a
  stack value as the return address (`[RET] return to 0x00515140 while 0xc5281df8 was expected`) and
  left every caller up the stack to the interpreter. New mid-hook heads (`0x514d63`) made it visible;
  the same pattern explains most of the hundreds of `[RET]` lines in earlier logs. `sa-recomp` now
  marks traces `noinline` while lifting (`keep_traces_separate`) and clears it after routing
  (`allow_trace_inlining`), so the compile step can still inline tail jumps. After the change a New
  Game run has no interpreter entries and 57 `[RET]` lines (all DRM thunks returning through
  `0xf00000xx`), down from 920.
  The M4 route still passes with no interpreter entries (24,450 frames, mean 40.2 ms, p99 68 ms,
  against 41.6 ms and 94 ms in M6), so keeping traces apart while lifting costs nothing measurable.
  M6's note that `--shadow` sees "calls that enter a trace in the middle of a function" is most
  likely this bug and should be rechecked.
- Remill's `MoveFunctionIntoModule` clears instruction metadata, so a marker on call instructions
  does not survive from lifting to the compile step; function attributes do.
- Widescreen Fix's `hook::pattern` scans to the end of the last executable section. The scanner used
  `SizeOfCode`, which stops before `.HOODLUM`.

Triage of the rest of `GTASA.FusionFix`:
- Every widescreen group is now decided. The CSprite2d vertex, `Draw`, `DrawRect`, `AddToBuffer` and
  radar-trace hooks are `replace`: they add the HUD scope's x offset (the mod's `CorrectX` is the
  identity at this commit), which the D3D9 2D offset does. The mode list, default resolution, the
  fix's in-game settings pages and rows, its event dispatch, raw mouse input and intro video placement
  are `replace` too (native mode list, launcher, host input and movie player). The canvas's
  transition and preview hooks are `drop`: they have no 1.0 US target.
- 13 rows stay unresolved, all in groups that are dropped, replaced or outside M8: CLEO (2), the
  RwIm2D render functions the D3D9 offset replaces (4), the canvas's transition and preview hooks
  (2), the mod's event hooks (2), the fix's settings rows (1) and the high frame rate timers (2).
- 23 groups are `pending` as FusionFix features outside M8's widescreen scope, moved to M11, each with
  a reason:
  controller button icons in text, the text outline option and the helper text's drop shadow,
  gamepad support, console gamma and other post effects, the transparent menu, a 256 px radar disc,
  the VCS camera shake, the disappearing-doors fix (a larger entry-exit list) and the high frame rate
  timing fixes.

Moved to M11: the HUD misalignment reported at 2560×1440 with 16:9 forced, and the FusionFix
features outside the widescreen port.

### M9 — SilentPatch

Goal: the kept SilentPatch fixes are ported and verified, one changelog section at a time.

Work:
- Port group by group, in changelog order.
- Renderer-touching fixes are reimplemented in the D3D9/Metal layer.
- The frame limiter and timing fixes are replaced natively, with options for 30, 60 and
  uncapped FPS.
- FLAC is dropped.
- For each group, record a reproduction of the original bug and the verification result in
  `tests/fixes/silentpatch/<group>.md`.

Exit criteria:
- Every group is marked ported, replaced or dropped, each with a reason.
- Every ported group has a passing reproduction test.
- No regressions on the M6 mission set.

Status: **in progress.** Every fix group is decided, the Critical-fixes section of the changelog is ported, and
the frame limiter is native. The Other-fixes and Enhancements sections are ported (batches 9 and 10), and batch 11
ported the groups deferred from earlier batches (moon phases, the parachute pipeline, the radio station change
animation, the biker cop drive-by, racing checkpoints, exploding car wheels) and five kept groups without a
changelog line that no batch had picked up. Every kept SilentPatch group now has a manifest entry over its sites.

Inventory and triage:
- The scanner missed SilentPatch's `HookEach_*(list, InterceptCall)` and `HookEach_*(list,
  InterceptMemDisplacement)` site lists, `AddVariableToReset` (the New Game resets), `*reinterpret_cast<…>(0x… + n)`
  table reads (the speech lookup tables) and the `VP::` spellings of the ModUtils calls. They are scanned now
  (`test_hook_each_lists_resets_and_table_reads`): 799 sites, up from 661, none unresolved. The one pattern
  inside the DRM-moved `DealWithNewPedPacket` is pinned by hand (0x156ee68).
- The scanner names a group after the nearest heading comment, which split or misnamed about 40 fixes ("The
  expected code:", "Fix original bugs", "Regular crosshair", the sun size hack under "Obtain a path to the ASI",
  true invincibility under the INI spelling comment). `mods/silentpatch/pins.yaml` moves those rows to the fix they
  belong to.
- `mods/silentpatch/triage.yaml`: 204 groups, none pending. 163 keep, 21 replace, 20 drop; 562, 103 and 134
  sites. Bindings and read-only helpers are dropped (natives name 1.0 US addresses directly); Windows display,
  input, DirectPlay, file-flag and semaphore fixes are replaced by the native layer; FLAC, SA-MP, SVF, the
  `[DoubleRearWheels]` override list and IMGs over 4 GB are dropped; SilentPatch's Small Steam texts option is
  replaced by the Widescreen Fix's subtitle scaling, which patches the same operands. Every group links its
  changelog line where there is one. No kept group overlaps a kept Widescreen Fix site.
- INI options become settings as groups are ported: `silentpatch.script_fixes` (on) so far; the sun size hack,
  coloured zone names, directional light from the sun, true invincibility, minimal HUD and sliding texts follow
  with their groups.

Ported (`manifest/silentpatch.yaml`, natives in `host/src/native/silentpatch.cpp`; one record per group in
`tests/fixes/silentpatch/`):
- Critical fixes: mirrors crash, collision data initialisation (car explosion crash), escalators crash, vending
  crash (temp objects), photo memory leak, replay crashes (clothes and body type), the delayed cigarette and
  bottle removal, Skimmer vehicles.ide defaults, trains in garages, timer fractions, zones bounds, the HOODLUM
  `CPlayerPedData::operator=`, the CdStream race, the gym glitch and the mission script fixes (basketball, Sweet's
  Girl, Mountain Cloud Boys, school cones, Air Raid, Ice Cold Killa, Supply Lines, Beefy Baron, quadruple stunt
  bonus), heap corruption, and the 14 ms frame delay.
- Other fixes, batch 2 (changelog order, first ~25 entries): mouse Y sensitivity and fade-in lock, corona Z test,
  lens flare, flare scaling, the streaming memory bug, bindable Num5, impound garages (four groups), last-bullet
  muzzle flash, timecyc.dat defaults and float multiplier, script sprite filtering, the 014C car generator
  counter, 31-bit randoms, help boxes with big messages, mirror buffers, car collision damage, stencil shadow pads
  and AI accuracy. Deferred within this stretch: patched `CAutomobile::Fix` (car panels), which needs frame
  resets and SilentPatch's SVF feature lists. The M9 route passes with batch 2 (timecyc reparses only
  RAINY_COUNTRYSIDE; animations use the fixed interpolator setup; flare, lens, impound-list, stencil and script
  sprite hooks run). M4 (24,450 frames, no interpreter entries, mean 33.0 ms), M5, M7 and M8 pass with batch 2.
  M5 first showed one interpreter entry: the lens hook called `CSprite::InitSpriteBuffer2D` (0x70cfd0), which is
  not a lifted trace head. The hook now does that function's three stores itself.
  One M9 run measured 86–98 ms frames at 30 and 60 fps from frame 1900 on (8 ms while loading); the next run, with
  nothing changed, measured 33.33 and 16.67 ms. The slow run is not explained yet; the Mac was otherwise busy and
  may have throttled the hidden window.
- Other fixes, batch 3 (changelog order, metric constants through the passenger-side car steal): metric-imperial
  constants and vehicle light randomness (the first SilentPatch `operand_slot`s), audio utility timers, interior car
  generators, RC vehicle streaming, the recruit triangle, SCM weapon ammo, bicycle fire, corona rotation, static and
  stored shadow alpha, extras reset and extra6, in-car mouse pitch, Quadbike wind and handlebar, the photo
  crosshair, lapdm1 biker cops, vehicles exploding twice, 16-bit script randoms, shooting stars, plane spawn sectors,
  riots and the passenger-side steal. SilentPatch's lapdm1 fix repoints one jump table entry that the game shares
  with three other cases, so the port hooks the shared target and checks the case. Deferred from this stretch, as
  they need deeper task, animation or RenderWare structures: the biker cop drive-by, racing checkpoints, the radio
  station change animation, the parachute pipeline, exploding car wheels and the New Game resets. The M9 route
  passes with batch 3: the audio utility clock advances 19,998 ms over 19,998 ms of frames; corona rotation and the
  shadow alpha hooks run; 16-bit randoms, plane spawn sectors and the interior car generators are not reached by
  the New Game route. M4 (24,450 frames, no interpreter entries, mean 32.4 ms), M5, M7 and M8 pass with
  batch 3.
- Other fixes, batch 4 (changelog order, credits scaling through gang war blips, plus the line-wrap group
  that has no changelog line):
  - Resolution scaling: credits, mission title and 'Mission Passed' durations, heat haze and underwater
    ripple, heat-seeking and gamepad crosshairs, Map screen cursor and limits, text box padding and menu
    line wraps.
  - Gameplay and models: nitrous while reversing, Hydra thrusters, the Hunter door, jumping on a bike from
    the front, rear van doors, cops' one-handed stance, roadblock clones, the gang tag count in saves,
    `CREATE_BIRDS`, the second player's blip and gang war blips.
  - SilentPatch takes its UI scales from the multipliers of game instructions (`Render2dStuff`,
    `StretchX`/`StretchY`, `CFont`, `CReplay`). The natives read those operands the same way, so they pick
    up the Widescreen Fix's operand and data slots. The two mods compose as they do on Windows: the
    credits print at 1.5 × 1.5 at 1280×720 with the Widescreen HUD scale.
  - Where SilentPatch recalculates its constants at a call, the port uses operand slots. These are
    refreshed every frame and whenever the Widescreen menu canvas applies or restores.
  - Three of SilentPatch's four `SetCurrentVideoMode` wrappers are already Widescreen Fix call_redirects,
    so the post-effects fix hooks the callee's `ret` instead.
  - The gang war check is in DRM-moved code. The scanner took the pattern's first match in the image, so
    its three sites are pinned to the moved body (0x1560b5b).
  - SilentPatch's optional sliding texts (`SlidingMissionTitleText`, `SlidingOddJobText`) move to the
    Enhancements batch.
  - The M9 route passes with batch 4. It starts the credits with `--call` and loads the Hunter, and checks
    the fixed-height credits layout, the scaled print and the `door_lf` unmark. The post-effects and menu
    wrap hooks run at boot and in the menu. The crosshair, map, big message, ripple, nitrous, thruster and
    gang war hooks are not reached by the New Game route. M4 (24,450 frames, no interpreter entries, mean
    32.2 ms), M5 (no interpreter entries), M7 and M8 pass with batch 4.
- Other fixes, batch 5 (changelog order, the speech system fixes through stats.html, plus the roadblock order
  that has no changelog line):
  - Speech: the lookup tables, weather conversations, criminals fleeing cops, ped voice names, turf takeover
    cheers, the solicit mood, Vagos taunts, gang attack lines, the CRASH_GENERIC fallback, PAIN_SPRAYED, CJ's
    CHASED replies, SHOOT lines with mouse and keyboard, SURROUNDED and COVER ME, 'Where are you from'
    replies and DRUGGED_IGNORE.
  - Also: the Good Citizen Bonus, shadows on rotated objects, the radio station name wrap, Securicars, the
    Fort Carson Beagle, Rhino lights, the stats.html headings and the roadblock law enforcement order.
  - SilentPatch patches the speech lookup tables at load. The port does the same work once in a mid_hook at
    WinMain (0x748710), with the context numbers from SilentPatch's `SpeechContextsSA.h`.
  - SilentPatch's injected calls with flag tests (Good Citizen, the SHOOT target) become mid_hooks that pick
    their resume address. Cops' dummy ped group is a zeroed block the size of a `CPedGroup` instead of
    SilentPatch's module base.
  - `--call` arguments can be `$TEXT` (a guest string) and `@` / `@+N` (the previous call's result).
  - The M9 route passes with batch 5. It checks at load the speech tables, Maccer's voice slot and the
    four peds.ide voice fixes. It streams `countn2_stream1.ipl` (`FindIplSlot`, then resource 25255 + slot)
    and checks that the Beagle generator becomes a BMX. The shadow transform runs in New Game; the
    conversation, gang, crash, spray, cop, Good Citizen and radio hooks are not reached by the route. M4
    (24,450 frames, no interpreter entries, mean 32.7 ms), M5 (no interpreter entries), M7 and M8 pass with
    batch 5.
- Other fixes, batch 6 (changelog lines 153–164, detached parts through stats in kilograms):
  - Detached part colours, culling and lighting; Map screen seams; Phoenix, Sweeper, Newsvan and plane
    flap animations; engine components that stop with the engine; the firela ladder; chained artict3
    trailers; hierarchy typos (DFT-30, Dumper, Uranus); the Tug tow bar; stats in kilograms.
  - **`vtable_redirect`.** SilentPatch replaces `CAutomobile::PreRender`, `CPlane::PreRender` and
    `CTrailer::GetTowBarPos` in their vtables. The DRM scatters their epilogues, so hooking their returns is
    fragile. The new manifest kind (§3.10) points the vtable slot at a native registered at
    `0xE0000000 + slot` and keeps the old pointer for `redirect_original()`, as a `call_redirect` does.
  - SilentPatch's stock SVF model list (11 models) is a native table, and its `SetComponentRotation` and
    `CMatrix` multiply work on the frames' modelling matrices. The deferred `CAutomobile::Fix` group needs
    the same table.
  - The kg format string is rewritten in place (`data_poke`), as only the two stats pages use it.
  - The M9 route passes with batch 6. `CAutomobile::PreRender` runs through its vtable slot (0x871164).
    Loading the DFT-30, Uranus and Dumper accepts five typo'd nodes (`wheel_lm`, `tailights`,
    `tailights2`, `transmision_f`, `transmision_r`). The part, animation, ladder and tow bar hooks are not
    reached by the route. M4 (24,450 frames, no interpreter entries, mean 32.5 ms), M5 (no interpreter
    entries), M7 and M8 pass with batch 6.
- Other fixes, batch 7 (changelog lines 165–175, 16:9 resolutions through the light beam):
  - Road reflections (RefFix), the light material restore, blown-up cars, dirty cars getting clean, unique
    and custom number plates (bikes included) with bilinear filtering, and the vehicle light beam's
    render states. 16:9 resolutions are already the native mode list's (M6, replace).
  - Weapon rendering (line 167) is deferred to the batch with line 193: SilentPatch reimplements
    `CPed::RenderWeapon` for it, and the weapon and jetpack shadow fix uses the same function.
  - SilentPatch keeps extra dirt materials in a `new[]` array; the port keeps them in a host-side list
    keyed by model info and drops it from `CVehicleModelInfo::Shutdown` (a `vtable_redirect`). The light
    beam's state save and restore hook the `call [reg+0x20]` instructions instead of pointing the engine
    operands at fake push/pop functions.
  - The M9 route passes with batch 7. New Game reaches the dirt lists (the first model has 20 dirt
    materials, more than the game's 16), dirt remaps, per-vehicle plates, the material restore and the
    light beam restore. Road reflections and blown-up cars are not reached.
  - M4 and M5 first showed one interpreter entry: the plate native called
    `CCustomCarPlateMgr::GetMapRegionPlateDesign` (0x6fd7a0), which the game only inlines, so it is not a
    lifted trace head. The native now reads the map region (0xc81314) itself. M4 (24,450 frames, no
    interpreter entries, mean 32.2 ms), M5 (no interpreter entries), M7 and M8 pass with batch 7.
- Other fixes, batch 8 (changelog lines 176–194 except weapon rendering and shadows, plus the deferred
  `CAutomobile::Fix` and `FuckCarCompletely` from line 52):
  - Lights on vehicles and peds (8 lights, no extra D3DLIGHT), the police scanner names, the bigger streamed
    entity list, mouse steering in the air, the vehicle repair fixes (`CAutomobile::Fix`, `CPlane::Fix`,
    `FuckCarCompletely`: misc parts, Towtruck hitch, plane doors and props, Rhino wheels) and the weapon icon
    on a jetpack. Lines 177, 178 and 185–186 were ported with the light beam, timecyc and impound groups;
    User Tracks and PCM WAVE (181–183) are replaced by the native audio layer.
  - SilentPatch rewrites `CAutomobile::Fix`'s epilogue into a tail jump to its own `Fix`. The port does
    that work in a mid_hook (frames from the model's clump, panels with the Towtruck and Tractor exceptions,
    Rhino wheels) and pops the saved registers itself before the SEH epilogue.
  - Moon phases (176) were deferred to batch 11: SilentPatch builds its mask texture with RenderWare's image and
    raster internals and a code patch to `RtPNGImageRead`, which the lifted build can't use.
  - The M9 route passes with batch 8; the repair hooks are not reached by New Game.
  - **D3D9 lights above index 7.** SilentPatch's light fix moves the car pipeline's special light from
    D3D light 1 to light 8, so it stops overwriting a real light. The D3D9 layer ignored indices of 8 and
    above. RenderWare still tracked light 8 as set and enabled, and CJ's lighting began to depend on what
    was drawn before him: M8's Hor+ comparison differed by 1.1–1.3% on him, varying between runs. The
    layer now stores 16 lights, as D3D9 accepts any index. Lights 0–7 keep their shader slots and a
    higher enabled light takes the first free one, as a driver with 8 active lights does
    (`test_lights_above_eight`). Two M8 runs then matched within 0.08–0.69%. With that layer, M9, M4
    (24,450 frames, no interpreter entries, mean 32.2 ms), M5 (no interpreter entries), M7 and M8 pass with
    batch 8.
- Other fixes, batch 9 (changelog lines 167 and 193, the weapon rendering deferred from batches 7 and 8, and
  lines 195–207):
  - Weapons are drawn with the ped, before the vehicle alpha pass, so they show through car windows, and
    `RenderWeaponPedsForPC` keeps only the muzzle flashes with the ped's lighting removed afterwards. A native
    `CPed::RenderWeapon` (hand bone, parachute offset, twin pistols) is shared with the shadow fix: a native
    `CShadowCamera::Update` takes the entity and draws a ped's weapon and jetpack into its shadow.
  - New Game resets (respray and bomb flags, dispatch timers, stat counters, the quit-car help text, the mad
    driver timer, the death and arrest penalties, text IPL pickups, car generators and stunt jumps), the
    `SEAROADER` cheat, looping loading screens, passenger lines when running a ped over, Pay 'n Spray cleaning
    behind the doors, paint jobs kept in garages, blurred rotors and propellers (`silentpatch.rotor_fix`) and dual
    rear wheels. Line 207 (quadruple stunt bonus) was ported with the script fixes.
  - **`imm_redirect`.** SilentPatch writes its rotor render callbacks into `push imm32` and `mov [esp+N], imm32`
    operands. The new manifest kind points such an operand at a native at lift time and keeps the old pointer
    for `redirect_original()` (§3.10).
  - The M9 route spawns a Hunter next to CJ (`CCheat::VehicleCheat`) while he holds the camera, and checks
    the static rotor callback, the rotor alphas, the blended rotor, the weapon drawn with the ped and the
    shadow camera update. New Game from the boot menu does not reach `CGame::InitialiseWhenRestarting`, so the
    resets are converted but not reached; plane propellers, trailers, loading screen loops, passenger lines and
    the paint job fallback are not reached either.
  - M9, M4 (24,450 frames, no interpreter entries, mean 32.15 ms), M5 (no interpreter entries), M7 and M8 pass
    with batch 9; M8's Hor+ comparison matched within 0.03–0.17%.
- Enhancements, batch 10 (changelog lines 212–226):
  - Kept: 6 extra directional lights on Medium and higher Visual FX (always on, as SilentPatch), and the
    sun size hack (`silentpatch.sun_size_hack`, on). Optional and off, as in SilentPatch's INI:
    - coloured zone names (`silentpatch.coloured_zone_names`);
    - sun reflections following the time of day (`silentpatch.directional_from_sun`);
    - True Invincibility (`silentpatch.true_invincibility`);
    - the Minimal HUD (`silentpatch.minimal_hud`; its two float alpha fixes are always on);
    - sliding mission titles and odd job texts (`silentpatch.sliding_mission_title`,
      `silentpatch.sliding_odd_job_text`).
  - Already ported or replaced: desktop resolution, the monitor dialog, the intro splashes, Small Steam
    texts and metric/imperial units. The German censorship and Steam save fixes patch only the Steam and
    RGL executables, so they have no 1.0 US sites; IMGs over 4 GB and FLAC are dropped.
  - SilentPatch points instruction operands at its own variables for the sun size hack and the sun
    direction. The port pushes the value at the sun's `fld`, and loads the three components at the first
    of the three `mov`s that read the vector, as mid_hooks that resume after them.
  - The M9 route turns every optional enhancement on in its uncapped run and checks that the sun direction
    and the minimal HUD mode run only there; the sun size hack, the extra lights and the float alpha fixes
    are reached in every run. Coloured zone names, True Invincibility and the sliding texts are converted but
    not reached. M9, M4 (24,450 frames, no interpreter entries, mean 32.14 ms), M5 (no interpreter entries),
    M7 and M8 pass with batch 10; M8's Hor+ comparison matched within 0.05–0.12%.
- Deferred groups, batch 11 (changelog lines 82, 91, 95, 97, 102 and 176):
  - Moon phases. SilentPatch's `lunar64.png` (MIT, by aap) is bundled in `mods/silentpatch/` and compiled
    into `sa-host` (CMake writes its bytes to a generated include). It is decoded with ImageIO and copied
    into a 64×64 8888 raster made with `RwRasterCreate` and `RwRasterLock`. The mask sprite is drawn with it
    into the alpha channel only (`D3DRS_COLORWRITEENABLE`), as SilentPatch does.
  - The parachute's skin pipeline is no longer replaced by the building pipeline. The radio station change
    animation keeps only the head and right arm, and is skipped in the Kart. Biker cops abort a drive-by
    sequence when the wanted level is gone (the DRM-moved call at 0x1561366; task types and `MakeAbortable`
    through the tasks' vtables). Racing checkpoints modulate their material colour. Exploding cars spawn the
    wheels the damage manager detached, and the random pick reaches the rear right wheel (an `operand_slot`
    for -4).
  - The M9 route checks the moon. It sets the clock to 3:40, the moon's brightest time (the moon shows from
    0:00 to 7:20), advances eight days and pins the camera at the moon. In a capture the masked moon is
    gibbous where the stock moon is full. The other five groups are converted but not reached by New Game.
  - A check of every kept group against the manifest found five that no batch had picked up, all without a
    changelog line (bomb ownership is line 75). They are now ported:
    - mipmapped weapon icon filtering;
    - `CVehicleModelInfo` initialisation (material lists cleared, the removed plate material pointed at a
      dummy);
    - bombs restored with their owner on stored cars and bikes (two DRM-moved garage loops);
    - whole-name `handling.cfg` matching;
    - the left rear light's damage state.
  - The biker cop native exposed a wrong `CVehicle` offset from batch 9: the driver is at 0x460 and the
    passengers at 0x464 (the game reads 0x460 45 times in its vehicle code). The passenger-line native read
    the passengers from 0x468; both use the right offsets now.
  - M9 (the moon in all three runs; the model info and handling fixes at load; no interpreter entries), M4
    (24,450 frames, no interpreter entries, mean 32.14 ms), M5 (no interpreter entries), M7 and M8 pass with
    batch 11; M8's Hor+ comparison matched within 0.06–0.13%.
- **Frame limiter.** `manifest/native.yaml` `frame-limiter`: a mid_hook on the main loop's limiter test keeps
  vanilla's conditions (menu Frame Limiter, the forced limit, the audio engine's check) and sleeps on the host
  clock until the next frame instead of spinning. `silentpatch.frame_limit` is 30 (default), 60 or uncapped.
- `sa-host --call FRAME:ADDR:CONV[:ARG,…]` calls a game function from the guest thread and logs the result, for
  reproductions that call the patched function directly.

Scripted results (`tools/route/m9_silentpatch.py`, New Game at 30, 60 and uncapped fps):
- Frame pacing: mean 33.33 ms at 30, 16.67 ms at 60 and 8.33 ms uncapped (the 120 Hz display), against about
  40 ms (25 fps) in M8's runs with the 14 ms delay.
- Timers: the game clock tracks wall time within 0.1% at every rate (1.0001, 0.9992, 0.9998). Truncating the same
  frames to whole milliseconds, as vanilla does, gives 0.985, 0.965 and 0.919.
- Zones: `GetCurrentZoneLockedOrUnlocked(9000, 9000)` returns unlocked; vanilla reads byte 0xba37ed, 189 bytes
  into a 100-byte grid, which holds 0 (locked) here.
- Script fixes: New Game's INITIAL gets the basketball save point move (7 values) and the stunt bonus patch; with
  `silentpatch.script_fixes=false` neither happens.
- vehicles.ide: the Skimmer and the ten boats have 11 of 15 fields and get the wheel defaults instead of stack
  contents.
- Photo: after a photo with the camera (`--call` to `CPed::GiveWeapon`), `psGrabScreen`'s surface is released
  with 0 references left.
- New collision data goes through the clearing allocator, and CdStream syncs wait through the locked path.
- M7's route still passes (melee ammo, jetpack hover, units, resolution string).
- Regressions: M4's route passes with no interpreter entries (24,450 frames, mean 32.3 ms, p99 41.9 ms; M8 measured
  38.6–42.5 ms with the 14 ms delay); M5's route passes (intro cutscene with audio, every stream opened); M8's route
  passes. M8's Hor+ comparison first failed at 1.1–1.2%: at 30 fps, frame 1850 falls while the "Use UP, DOWN…"
  help box is still up, and the box sat inside the compared region in the 4:3 picture (at the left edge in the wider
  ones, as it should). The region now starts below it (`SCENE` y 120); the middles match within 0.01–0.04%.

Not reproduced yet (ported, converted and loaded, but no scripted reproduction): mirrors crash, heap corruption,
delayed prop destruction, escalators, vending, replay crashes, trains in garages, `CPlayerPedData::operator=`,
the CdStream race itself, the gym glitch and the per-mission script fixes. Each record in `tests/fixes/silentpatch/`
says what its reproduction needs.

Findings:
- The intro cutscene skip in `Route.new_game()` counts frames, so at 60 fps and above the route reaches first
  control later in game time; post-control checks that need CJ (the photo) run only at 30 fps for now.
- Giving a weapon with `CPed::GiveWeapon` does not stream its model; aiming an unloaded camera crashes in the
  animation code. The route requests model 367 first, as scripts do.

### M10 — Project2DFX

Goal: LOD lights, extended draw distance and distant cars, at acceptable performance.

Work:
- Port the LOD corona logic. Its data files (GPL-3.0 mod data) go in
  `mods/project2dfx/data` with notices, served through the VFS overlay.
- First native RW overrides (§3.6 Phase B): batched Im3D corona rendering.
- Draw/LOD distance and distant-car settings exposed in the launcher.

Exit criteria:
- Night corona counts and positions match the reference at fixed views.
- Frame-time budget holds on base M1 at default settings (budget set from the M6 baseline).

### M11 — Performance & polish

Goal: a stable 60 fps on a base M1.

Work:
- Profile with Instruments and os_signpost. Find hot lifted functions.
- Lifter optimizations: flag elimination, x87 promotion coverage, devirtualization of hot
  indirect calls, PGO from playback runs.
- Native RW overrides and native Metal pipelines for the hottest paths. Shrink the D3D9 layer
  where it shows up in profiles.
- Audit high-frame-rate gameplay issues that remain after the SilentPatch fixes, and gate them
  behind the frame-limiter setting.
- **Controller support (moved from M5).** What exists, from M5 work:
  - DInput8 always enumerates one game controller (`GetObjectInfo`, `DIPROP_RANGE`, `Poll`,
    `DIJOYSTATE2`). SA then applies its default joypad layout at boot.
  - SA's generic default layout is PS2-ordered: 1 Circle, 2 Cross, 3 Square, 4 Triangle,
    5 L2, 6 R2, 7 L1, 8 R1, 9 Select, 10 L3, 11 R3, 13–16 d-pad up/right/down/left. A vendor
    0x3427 / product 0x1190 pad gets a second layout (1 Triangle … 4 Square). Buttons are reported
    in the generic order by position, so A/Cross is Cross on both pads.
  - X/Y are the left stick, Z/Rz the right stick (SA reads the right stick only when both
    exist). The POV hat is reported centred, because SA treats it as a second left stick.
  - SA never maps Start; the pause menu opens only on Escape. The Menu/Options button sends Escape.
  - GameController.framework snapshots the extended gamepad on its own queue and follows connect
    and disconnect (`[INPUT] controller connected`). Disconnected means a neutral pad.
  - `--keys` drives a virtual pad: `pad.a` … `pad.right`, `lstick@X:Y`, `rstick@X:Y` (percent).
    Scripted runs started New Game, paused and navigated menus, walked, rode, got on and off the
    BMX and turned the camera.
  - A `gta_sa.set` saved while no joystick existed stores an empty joypad table, which overrides
    the defaults (as on Windows). Controller Setup → Restore Defaults or deleting the file fixes it.

  Still to do: test with real Xbox and DualSense controllers (drive, walk, aim, menus),
  unplug/replug mid-game, decide whether the controller should be enumerated when none is
  connected, and rumble.
- **Reference check (moved from M7).** Set up the Wine/CrossOver reference: 1.0 US, an ASI loader and
  the three mods at the commits pinned in `tools/mod-inventory/mods.json`. Pin the same camera views
  with a CLEO script (`SET_FIXED_CAMERA_POSITION`, `POINT_CAMERA_AT_POINT`, `RESTORE_CAMERA`, the
  coordinates in `VIEWS` of each route), capture them and diff with `--reference DIR`
  (`tools/verify/image_diff.py`). Confirm the ported fixes from M7–M10 behave as on Windows; this
  needs a person to play the reference and a Windows-side way to read the same watch values.
- **HUD misaligned at 2560×1440 with 16:9 forced (moved from M8).** Reported from a playthrough:
  2560×1440 with 16:9 forced. The top-right HUD is out of place:
  - the clock and the fist icon sit high;
  - there is a large gap between the clock and the health bar;
  - the money text runs far left of the health bar, under the weapon icon.

  Right edges still line up (clock, health bar and money end together). The M8 route checks the health
  bar and money only at 720 high and with `widescreen.hud_aspect` at 16:9 on a 21:9 window, so
  1440-high windows and the clock and weapon icon are not covered. To do:
  - Reproduce at 2560×1440 and confirm which setting "16:9 forced" was (`widescreen.hud_aspect`,
    `widescreen.menu_aspect` or the window).
  - Compare against the Widescreen Fix on Windows.
  - Fix it, and extend `tools/route/m8_widescreen.py` with a 1440-high run that checks the clock and
    weapon icon too.
- **FusionFix features outside the widescreen port (moved from M8).** `GTASA.FusionFix` covers more
  than widescreen. These 23 groups stay `pending` in `mods/widescreen/triage.yaml` until each is
  ported or dropped:
  - controller button icons in text (5 groups);
  - the text outline option and the menu helper text's drop shadow (4);
  - gamepad support: modern aim, vibration, camera style (1), with the controller work above;
  - the high frame rate timing fixes (6), with the high-frame-rate audit above;
  - console gamma and other post effects (3);
  - the transparent pause menu (1);
  - a 256 px radar disc texture (1);
  - the VCS camera shake (1);
  - the disappearing-doors fix, a larger visible entry-exit list (1).
- **Basic trainer.** Native host code, off by default: `--trainer` on `sa-host` and a launcher
  toggle (the launcher config gains a key, so its decoding must tolerate configs without it).
  It runs once per presented frame on the game thread (from `dev_present`), reads hotkeys with
  `input::vk_down` edge detection, and does nothing while the player ped is missing or the
  frontend menu is open (`FrontEndMenuManager.m_bMenuActive`, `0xBA67A4`). Scope:
  - F1 refills health (`CPed` +0x540 to the max at +0x544) and armour (+0x548).
  - F2 adds $100,000 to `CPlayerInfo::m_nMoney` (`0xB7CE50`), capped at 999,999,999.
  - F3 clears the wanted level with `CWanted::SetWantedLevel(0)` (`0x562470`, thiscall), the
    `CWanted*` reached through player ped +0x444 → `CPlayerPedData` +0.
  - F4 repairs the current vehicle (`CPed` +0x58C while the in-vehicle flag, bit 8 of +0x46C,
    is set) by setting `CVehicle` health (+0x4C0) to 1000.
  - F5 toggles god mode (F1 and F4 applied every frame); F6 toggles never wanted (F3 applied
    whenever the level is above 0).
  - Feedback through `CHud::SetHelpMessage` (`0x588BE0`, cdecl) from a persistent guest buffer.
  - Player ped from `CWorld::Players[0].m_pPed` (`0xB7CD98`).

  Out of scope for the basic version: weapons and vehicle spawning (they need model streaming),
  teleport to the map marker (needs scene loading and ground height), and full damage repair
  through the vehicle vtable. The struct offsets are from the community SA 1.0 US layout and the
  function entries are confirmed in `analysis/functions.json`; each action still needs a scripted
  check (`--keys` with `--watch`, starting from the M4 New Game route).

Exit criteria:
- 1080p at 60 fps with 99th-percentile frame time under 20 ms on base M1, across the fixed
  benchmark routes (city, countryside, night with 2DFX).
- No `[INTERP]` hits in normal gameplay.
- Xbox and DualSense controllers drive, walk, aim and navigate menus. Unplugging and
  replugging mid-game works.
- With `--trainer`, each trainer hotkey has its effect in a scripted run, and without it the
  F-keys change nothing.
- The top-right HUD lines up at 2560×1440 with 16:9 forced, checked in a scripted run.
- Every FusionFix group in `mods/widescreen/triage.yaml` is kept, replaced or dropped.

### M12 — Release

Goal: a public, GPL-compliant release that contains no game content.

Work:
- Notarized app bundle: launcher, `sa-host`, `sa-recomp` with bundled LLVM/lld, and runtime.
  Corresponding source for every GPL component.
- First-run UX: locate the game, verify, explain the downgrade path, convert, play.
  Reconvert on update.
- Crash report export with no game bytes and no user paths.
- User documentation.

Exit criteria:
- A clean macOS 26 machine goes from download to in-game.
- The release artifacts pass the repo guard checks (no game bytes).
- `THIRD_PARTY_NOTICES.md` is complete.

---

## 5. Mod Porting Process

Unchanged in spirit from the original plan; the tooling is in M7.

1. **Inventory** with the `tools/mod-inventory` script. Don't read mods top to bottom.
2. **Pin** every site to a 1.0 US address and drop other-version branches.
3. **Triage**:
   - Keep game logic, physics, timing math, HUD and camera.
   - Replace natively anything about D3D9, DSound, window, monitor or frame limiter.
   - Drop Windows/driver quirks, INI parsing and other-version code.
4. **Group by fix.** Port or drop whole groups.
5. **Port**: injection calls become manifest entries. C++ logic compiles natively against
   guest-memory accessors. Naked asm becomes `GuestCtx&` functions.
6. **Renderer-touching patches** are flagged by the conflict check and reimplemented in gfx code.
7. **Verify** with the M7 harness against the reference.

Order: Widescreen Fix → SilentPatch (one changelog section at a time) → Project2DFX.

---

## 6. Testing

- **Reference environment:** 1.0 US + ASI loader + the original three mods under CrossOver/Wine
  on the same Mac. This is ground truth for behavior and screenshots.
- **Deterministic replay:** virtualized time (QPC, `timeGetTime`, `GetTickCount` driven by a
  replay clock) plus recorded input and fixed saves. Checkpoints compare player and vehicle
  state and screenshots.
- **Differential function tests:** lifted vs interpreter on randomized inputs for pure
  functions (math, matrix, collision helpers). Disagreements point at lifter or x87 bugs.
- **Interpreter coverage:** every replay run reports `[INTERP]` addresses. The target is zero
  from M4 onward.
- **Unit tests:** x87 lowering and precision control, calling-convention wrappers, manifest
  parsing and conflict checks, VFS case mapping, Win32 sync semantics, D3D9 state →
  pipeline-key mapping, shader translator golden outputs (synthetic shaders only, never shaders
  from the exe).
- **Per-fix verification** (M7+): reproduce the bug, apply the fix, compare against the
  reference at fixed saves and cameras.
- CI runs everything that doesn't need the game. Tests that need the exe run locally or on a
  self-hosted runner that has the user's own copy, and never upload artifacts.

---

## 7. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| Obfuscated code in `.text`(2) / `.HOODLUM` defeats analysis | Missed or wrong functions | `overrides.yaml`, interpreter fallback, `[INTERP]` feedback loop |
| x87 precision divergence | Physics/AI drift, rare bugs | Precision-control tracking, differential tests, reference replays |
| ARM memory ordering vs x86 TSO | Rare streaming/audio corruption | Barriers at sync points, `tso` markers, serialize debug mode |
| Remill flag/State overhead | Low FPS | Custom passes, PGO, native overrides for hot paths (M11) |
| Conversion time and size on user Macs | Bad first-run UX | Parallel sharding, progress UI, cached outputs, stamp-based reconversion |
| D3D9 surface wider than expected | Slow progress at M2–M4 | Method-coverage logging, implement only what is called |
| Local dylib loading blocked by platform policy | Launch failure | Ad-hoc signing + library-validation entitlement, tested on clean machines each release |
| Mid-function mod patches assume x86 layout | Incorrect hooks | Instruction-boundary validation, native reimplementation |
| GPL compliance mistakes | Legal exposure | Notices, source offer, license review before M12 (not legal advice) |
| Game-content leakage into repo or release | Legal exposure | Repo guard (M0) on commits and release artifacts |

---

## 8. Repository Layout

```
/launcher                 Swift app: verify, convert, settings, launch
/host                     sa-host executable: guest region, dispatch, manifest engine
/interp                   x86-32 interpreter (Zydis-based)
/tools/recomp             sa-recomp, LLVM passes, Remill integration
/tools/recomp/ghidra      Ghidra headless export scripts (dev only)
/tools/mod-inventory      patch-site extraction
/tools/repo-guard         pre-commit + CI content checks
/tools/pe-inspect         PE inspection, sa-verify
/analysis/sa-1.0us        committed address-only analysis data + overrides
/manifest                 hook manifest YAML (native, silentpatch, widescreen, project2dfx)
/runtime/loader           PE mapping, imports, TEB/PEB, CRT support
/runtime/win32            kernel32, user32, advapi32, winmm, ws2_32, ole32
/runtime/gfx              D3D9 subset on Metal, shader translator, native RW overrides
/runtime/audio            DSound8/EAX on CoreAudio, vorbisfile
/runtime/input            DInput8, WM_* synthesis, GameController
/runtime/vfs              path mapping, overlays, overlapped I/O
/mods/silentpatch         ported native code
/mods/widescreen          ported native code
/mods/project2dfx         ported native code + data
/tests                    unit, differential, replay, per-fix
/third_party              pinned submodules (Remill, Zydis, libvorbis, pl_mpeg)
LICENSE                   GPL-3.0-or-later
THIRD_PARTY_NOTICES.md
```

---

## 9. Open Questions

- Licenses of community symbol maps (plugin-sdk, gta-reversed) before committing anything
  derived from them. Until then they are local-only for debugging.
- EAX reverb fidelity: ignore, approximate, or emulate presets.
- Whether to report SSE/SSE2 in CPUID (steers RW math paths). Decide after differential testing.
- Movie playback: confirm the `CoCreateInstance` CLSIDs before choosing the native replacement
  or fake DirectShow.
- Which keyboard path SA actually reads (WM_* vs DInput8), to confirm in M3.
