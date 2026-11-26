#!/usr/bin/env python3
import argparse
import json
import re
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

LLVM_MC = "/opt/homebrew/opt/llvm@22/bin/llvm-mc"
MAX_BYTES = 512
FORBIDDEN = re.compile(r"^(call|lcall|ljmp|int|iret|in|out|ins|outs|lods|stos|movs|cmps|scas|rep|hlt|cli|sti|lock|"
                       r"push\s+(cs|ds|es|ss|fs|gs)|pop\s+(ds|es|ss|fs|gs)|fldenv|fnstenv|frstor|fnsave|fxsave|fxrstor|"
                       r"femms|pf\w+|pi2f\w|pf2i\w|pswapd|pmulhrw|pavgusb)\b")
BRANCH = re.compile(r"^(j\w+|loop\w*)\s+(-?\d+)$")


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    table = pe + 24 + optional_size
    out = []
    for i in range(count):
        vsize, va, raw_size, raw_offset = struct.unpack_from("<IIII", data, table + 40 * i + 8)
        out.append((base + va, raw_size, raw_offset))
    return out


def read_va(data, secs, va, length):
    for start, raw_size, raw_offset in secs:
        if start <= va and va + length <= start + raw_size:
            return data[raw_offset + va - start:raw_offset + va - start + length]
    return None


def disassemble(code):
    src = " ".join(f"0x{b:02x}" for b in code)
    out = subprocess.run([LLVM_MC, "--disassemble", "-triple=i386", "-output-asm-variant=1", "-show-encoding"],
                         input=src, capture_output=True, text=True).stdout
    insns = []
    for line in out.splitlines():
        m = re.search(r"# encoding: \[(.*)\]", line)
        if not m:
            continue
        insns.append((line.split("#")[0].strip(), len(m.group(1).split(","))))
    return insns


def qualifies(entry, code):
    insns = disassemble(code)
    if sum(n for _, n in insns) != len(code):
        return False
    pc = entry
    x87 = 0
    returned = False
    for text, n in insns:
        if not text or FORBIDDEN.match(text) or "fs:" in text:
            return False
        if text.startswith("jmp") and not BRANCH.match(text):
            return False
        b = BRANCH.match(text)
        if b:
            target = pc + n + int(b.group(2))
            if not entry <= target < entry + len(code):
                return False
        if text.startswith("f"):
            x87 += 1
        if text.startswith("ret"):
            returned = True
        pc += n
    return returned and x87 >= 2


def main():
    p = argparse.ArgumentParser(description="list leaf x87 functions for lifted-vs-interpreter differential tests")
    p.add_argument("--exe", required=True, type=Path)
    p.add_argument("--functions", required=True, type=Path)
    p.add_argument("--out", required=True, type=Path)
    args = p.parse_args()
    data = args.exe.read_bytes()
    secs = sections(data)
    funcs = json.loads(args.functions.read_text())["functions"]
    candidates = []
    for f in funcs:
        if f.get("thunk") or len(f["ranges"]) != 1:
            continue
        lo, hi = (int(x, 16) for x in f["ranges"][0])
        entry = int(f["entry"], 16)
        if entry != lo or hi - lo + 1 > MAX_BYTES:
            continue
        code = read_va(data, secs, lo, hi - lo + 1)
        if code is None or not any(0xD8 <= b <= 0xDF for b in code):
            continue
        candidates.append((entry, code))
    with ThreadPoolExecutor() as pool:
        keep = [e for (e, c), ok in zip(candidates, pool.map(lambda ec: qualifies(*ec), candidates)) if ok]
    args.out.write_text("".join(f"0x{e:08x}\n" for e in sorted(keep)))
    print(f"{len(keep)} of {len(candidates)} candidates written to {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
