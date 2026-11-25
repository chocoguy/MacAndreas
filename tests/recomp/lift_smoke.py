#!/usr/bin/env python3
import argparse
import hashlib
import re
import struct
import subprocess
import sys
from pathlib import Path

SUPPORTED_SHA256 = "a559aa772fd136379155efa71f00c47aad34bbfeae6196b0fe1047d0645cbd26"
SKIP = 77

CASES = [
    (0x00809400, 12),
    (0x005807C0, 32),
]

HOST_TRIPLE = "arm64-apple-macosx26.0.0"


def find_exe(game_dir: Path):
    if not game_dir.is_dir():
        return None
    for entry in game_dir.iterdir():
        if entry.name.lower() == "gta_sa.exe" and entry.is_file():
            return entry
    return None


def sections(data: bytes):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    image_base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    table = pe + 24 + optional_size
    out = []
    for i in range(count):
        vsize, va, raw_size, raw_offset = struct.unpack_from("<IIII", data, table + 40 * i + 8)
        out.append((image_base + va, max(vsize, raw_size), raw_offset, raw_size))
    return out


def read_va(data: bytes, va: int, length: int) -> bytes:
    for start, size, raw_offset, raw_size in sections(data):
        if start <= va and va + length <= start + min(size, raw_size):
            offset = raw_offset + (va - start)
            return data[offset:offset + length]
    raise ValueError(f"0x{va:08x}+{length} is not file-backed")


def host_datalayout(clang: Path) -> str:
    out = run([str(clang), "-target", HOST_TRIPLE, "-S", "-emit-llvm", "-x", "c", "/dev/null", "-o", "-"]).stdout
    match = re.search(r'^target datalayout = "(.*)"', out, flags=re.M)
    if not match:
        sys.stderr.write("could not determine the host datalayout\n")
        sys.exit(1)
    return match.group(1)


def zero_state(ir: str) -> str:
    pattern = re.compile(r"^(\s*)(%[\w.]+) = alloca %struct\.State, align (\d+)\n", flags=re.M)
    ir, count = pattern.subn(
        lambda m: f"{m.group(0)}{m.group(1)}store %struct.State zeroinitializer, ptr {m.group(2)}, align {m.group(3)}\n",
        ir,
    )
    if count != 1:
        sys.stderr.write(f"expected one State alloca in the wrapper, found {count}\n")
        sys.exit(1)
    return ir


def retarget(ir: str) -> str:
    ir = zero_state(ir)
    ir = re.sub(r'^target datalayout = ".*"\n', "", ir, flags=re.M)
    ir = re.sub(r'^target triple = ".*"', f'target triple = "{HOST_TRIPLE}"', ir, flags=re.M)
    ir = re.sub(r'\s*"target-(cpu|features)"="[^"]*"', "", ir)
    ir = re.sub(r'\s*"tune-cpu"="[^"]*"', "", ir)
    return ir


def run(cmd, **kwargs):
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **kwargs)
    if result.returncode != 0:
        name = Path(cmd[0]).name
        sys.stderr.write(f"{name} failed with exit code {result.returncode}\n{result.stderr[-4000:]}\n")
        sys.exit(1)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Lift two gta_sa.exe functions with Remill and run them natively.")
    parser.add_argument("--remill-lift", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--host", type=Path, required=True)
    parser.add_argument("--layout-check", type=Path, required=True)
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()

    exe = find_exe(args.game_dir)
    if exe is None:
        print(f"skipping: no gta_sa.exe in {args.game_dir}")
        return SKIP
    if not args.remill_lift.is_file() or not args.clang.is_file():
        print("skipping: remill-lift or LLVM clang not available")
        return SKIP
    data = exe.read_bytes()
    if hashlib.sha256(data).hexdigest() != SUPPORTED_SHA256:
        sys.stderr.write(f"{exe} is not gta_sa.exe 1.0 US\n")
        return 1

    args.work.mkdir(parents=True, exist_ok=True)
    datalayout = host_datalayout(args.clang)
    modules = []
    for address, length in CASES:
        code = read_va(data, address, length)
        raw = args.work / f"sub_{address:x}.raw.ll"
        run([
            str(args.remill_lift),
            "--arch", "x86",
            "--os", "windows",
            "--address", hex(address),
            "--entry_address", hex(address),
            "--bytes", code.hex(),
            "--signature", "EAX()",
            "--ir_out", str(raw),
        ])
        print(run([str(args.layout_check), str(raw), datalayout]).stdout.strip())
        host = args.work / f"sub_{address:x}.ll"
        host.write_text(retarget(raw.read_text()))
        raw.unlink()
        modules.append(str(host))

    dylib = args.work / "lifted.dylib"
    run([
        str(args.clang), "-shared", "-O2", "-Wno-override-module",
        "-target", HOST_TRIPLE,
        "-undefined", "dynamic_lookup",
        "-o", str(dylib), *modules,
    ])
    for module in modules:
        Path(module).unlink()

    result = subprocess.run([str(args.host), str(exe), str(dylib)])
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
