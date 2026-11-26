#!/usr/bin/env python3
import argparse
import struct
import sys
from pathlib import Path

SAVE_SIZE = 202752
TAG = b"BLOCK"


def inspect(data):
    problems = []
    if len(data) != SAVE_SIZE:
        problems.append(f"size {len(data)} bytes, expected {SAVE_SIZE}")
    if not data.startswith(TAG):
        problems.append("does not start with a BLOCK tag")
    stored = struct.unpack_from("<I", data, len(data) - 4)[0] if len(data) >= 4 else None
    computed = sum(data[:-4]) & 0xFFFFFFFF
    if stored != computed:
        problems.append(f"checksum 0x{stored:08x} stored, 0x{computed:08x} computed")
    info = {}
    if len(data) > 0x200 and data.startswith(TAG):
        body = data[len(TAG):]
        info["version"] = f"0x{struct.unpack_from('<I', body, 0)[0]:08x}"
        info["title"] = body[4:104].split(b"\0")[0].decode("latin-1")
        x, y, z = struct.unpack_from("<3f", body, 0x70)
        info["camera"] = f"{x:.1f}, {y:.1f}, {z:.1f}"
    return problems, info, stored


def main():
    p = argparse.ArgumentParser(description="check GTA San Andreas PC save files (GTASAsfN.b)")
    p.add_argument("saves", nargs="+")
    args = p.parse_args()
    failed = 0
    for path in args.saves:
        data = Path(path).read_bytes()
        problems, info, checksum = inspect(data)
        status = "ok" if not problems else "INVALID"
        details = ", ".join(f"{k} {v}" for k, v in info.items())
        print(f"{path}: {status}, checksum 0x{checksum or 0:08x}" + (f", {details}" if details else ""))
        for problem in problems:
            print(f"  {problem}")
        failed += 1 if problems else 0
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
