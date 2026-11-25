# MacAndreas

## A native static recompilation port of GTA SA for macOS

MacAndreas converts your own copy of GTA: San Andreas (PC, `gta_sa.exe` 1.0 US) into a native
Apple Silicon program. SilentPatch, the Widescreen Fix and Project2DFX are built in. This
repository contains no game code or assets; see [ROADMAP.md](ROADMAP.md) for the plan.

Status: **M0 (foundations) complete**; M1 (lift & boot) is next. Nothing is playable yet.

### Requirements

- Apple Silicon Mac, macOS 26 or later
- Xcode command line tools, CMake 3.28+, Ninja
- Ghidra 12.2 (developers regenerating `analysis/` only)

### Build

```bash
git config core.hooksPath .githooks
```

```bash
cmake --preset dev
```

```bash
cmake --build --preset dev
```

```bash
ctest --preset dev
```

The recompiler tests also need Remill built against Homebrew LLVM 22
(`brew install llvm@22`). This clones Remill at a pinned commit into the git-ignored `.cache/`:

```bash
tools/recomp/setup-remill.sh
```

Tests labelled `game` need your own executable. By default they look in
`gamefiles/Grand Theft Auto San Andreas/`, which is git-ignored. Set `MACANDREAS_GAME_DIR` at
configure time to point elsewhere. Without the game they are skipped.

### Recompile and boot

`analysis/` is local-only, so export it first with `tools/recomp/ghidra/export.sh`. Then lift,
compile and link the executable into `build/dev/native/libgta_sa_native.dylib`. This takes
about 11 minutes:

```bash
build/dev/tools/recomp/sa-recomp/sa-recomp build --exe "gamefiles/Grand Theft Auto San Andreas/gta_sa.exe" --analysis analysis/sa-1.0us --manifest manifest --runtime build/dev/runtime/recomp/guest_intrinsics.bc --out build/dev/native
```

After a compiler-only change, rebuild the objects from the existing shards without lifting
again:

```bash
build/dev/tools/recomp/sa-recomp/sa-recomp recompile --runtime build/dev/runtime/recomp/guest_intrinsics.bc --out build/dev/native
```

Boot it. `--trace-imports` logs every API call:

```bash
build/dev/host/sa-host --game "gamefiles/Grand Theft Auto San Andreas" --native build/dev/native/libgta_sa_native.dylib --manifest manifest
```

Code the recompiler missed runs in the interpreter and is logged as `[INTERP] 0xADDR`. To turn
those addresses into lift roots for the next build:

```bash
tools/recomp/interp_triage.py --apply boot.log
```

### Tools

- `sa-verify <gta_sa.exe | game dir>` checks that the executable is the supported 1.0 US build.
- `pe-inspect [--json] <file.exe>` prints headers, sections and imports.
- `tools/recomp/ghidra/export.sh <gta_sa.exe>` regenerates `analysis/sa-1.0us/`.
- `tools/repo-guard/repo_guard.py` blocks game files, lifted code and binaries from commits.

### License

GPL-3.0-or-later. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
