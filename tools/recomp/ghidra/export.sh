#!/usr/bin/env bash
set -euo pipefail

EXPECTED_SHA256=a559aa772fd136379155efa71f00c47aad34bbfeae6196b0fe1047d0645cbd26

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../../.." && pwd)"

exe="${1:?usage: export.sh <path/to/gta_sa.exe> [out_dir]}"
out_dir="${2:-$repo_root/analysis/sa-1.0us}"

if [[ -z "${GHIDRA_HOME:-}" ]]; then
    GHIDRA_HOME="$(brew --prefix ghidra 2>/dev/null)/libexec"
fi
headless="$GHIDRA_HOME/support/analyzeHeadless"
if [[ ! -x "$headless" ]]; then
    echo "analyzeHeadless not found under $GHIDRA_HOME; set GHIDRA_HOME to the Ghidra install root" >&2
    exit 1
fi

actual="$(shasum -a 256 "$exe" | awk '{print $1}')"
if [[ "$actual" != "$EXPECTED_SHA256" ]]; then
    echo "refusing to analyze $exe: not gta_sa.exe 1.0 US (sha256 $actual)" >&2
    exit 2
fi

mkdir -p "$out_dir"
out_dir="$(cd "$out_dir" && pwd)"

work="$(mktemp -d "${TMPDIR:-/tmp}/macandreas-ghidra.XXXXXX")"
trap 'rm -rf "$work"' EXIT

"$headless" "$work" sa \
    -import "$exe" \
    -processor x86:LE:32:default \
    -cspec windows \
    -max-cpu "$(sysctl -n hw.ncpu)" \
    -scriptPath "$script_dir" \
    -postScript ExportAnalysis.java "$out_dir" \
    -deleteProject

echo "analysis written to $out_dir"
