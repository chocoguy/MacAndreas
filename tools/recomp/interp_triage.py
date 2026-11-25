#!/usr/bin/env python3
import argparse
import pathlib
import re
import sys

INTERP = re.compile(r"^\[INTERP\] 0x([0-9a-fA-F]{8}) caller=0x([0-9a-fA-F]{8})")


def read_roots(path):
    roots = set()
    if path.exists():
        for line in path.read_text().splitlines():
            text = line.split("#", 1)[0].strip()
            if text:
                roots.add(int(text, 16))
    return roots


def main():
    parser = argparse.ArgumentParser(
        description="Collect [INTERP] addresses from sa-host logs and propose them as lift roots.")
    parser.add_argument("logs", nargs="+", type=pathlib.Path, help="sa-host stderr logs")
    parser.add_argument("--analysis", type=pathlib.Path, default=pathlib.Path("analysis/sa-1.0us"),
                        help="analysis directory holding extra_roots.txt")
    parser.add_argument("--apply", action="store_true", help="append new addresses to extra_roots.txt")
    args = parser.parse_args()

    found = {}
    for log in args.logs:
        for line in log.read_text(errors="replace").splitlines():
            match = INTERP.match(line)
            if match:
                found.setdefault(int(match.group(1), 16), int(match.group(2), 16))

    roots_path = args.analysis / "extra_roots.txt"
    known = read_roots(roots_path)
    new = {addr: caller for addr, caller in sorted(found.items()) if addr not in known}
    for addr, caller in new.items():
        print(f"0x{addr:08x}  # first entered from 0x{caller:08x}")
    if not new:
        print("no new [INTERP] addresses", file=sys.stderr)
        return 0
    if args.apply:
        with roots_path.open("a") as out:
            for addr, caller in new.items():
                out.write(f"0x{addr:08x}  # [INTERP] from 0x{caller:08x}\n")
        print(f"appended {len(new)} root(s) to {roots_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
