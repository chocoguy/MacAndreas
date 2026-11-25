#!/usr/bin/env bash
set -euo pipefail

REMILL_COMMIT=66c488c9911fd9b368e4d0c9647d74239f8bba8b
LLVM_MAJOR="${LLVM_MAJOR:-22}"

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
src="$repo_root/.cache/remill"
llvm_prefix="$(brew --prefix "llvm@$LLVM_MAJOR")"

if [[ ! -d "$src/.git" ]]; then
    git init -q "$src"
    git -C "$src" remote add origin https://github.com/lifting-bits/remill.git
fi
if [[ "$(git -C "$src" rev-parse HEAD 2>/dev/null)" != "$REMILL_COMMIT" ]]; then
    git -C "$src" fetch -q --depth 1 origin "$REMILL_COMMIT"
    git -C "$src" checkout -q FETCH_HEAD
fi

cmake -G Ninja -S "$src/dependencies" -B "$src/dependencies/build" \
    -DUSE_EXTERNAL_LLVM=ON "-DCMAKE_PREFIX_PATH:PATH=$llvm_prefix"
cmake --build "$src/dependencies/build"

cmake -G Ninja -S "$src" -B "$src/build" \
    "-DCMAKE_PREFIX_PATH:PATH=$src/dependencies/install" \
    -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_C_COMPILER=$llvm_prefix/bin/clang" \
    "-DCMAKE_CXX_COMPILER=$llvm_prefix/bin/clang++" \
    "-DCMAKE_OSX_SYSROOT=$(xcrun --show-sdk-path)" \
    "-DCMAKE_INSTALL_PREFIX=$src/install" \
    -DREMILL_ENABLE_TESTING=OFF
cmake --build "$src/build"
cmake --install "$src/build"

echo "remill-lift: $src/install/bin/remill-lift-$LLVM_MAJOR"
