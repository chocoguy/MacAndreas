#!/usr/bin/env python3
import argparse
import os
import re
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Tuple

MAX_FILE_BYTES = 2 * 1024 * 1024

DENIED_EXTENSIONS = {
    ".exe", ".dll", ".asi", ".img", ".txd", ".dff", ".col", ".ifp", ".gxt", ".scm", ".mpg",
    ".ll", ".bc", ".o", ".obj", ".a", ".dylib", ".so", ".metallib", ".shadercache", ".pyc",
}

DENIED_PREFIXES = ("gamefiles/",)

ANALYSIS_PREFIX = "analysis/"

ANALYSIS_FORBIDDEN_KEYS = re.compile(
    r'"(bytes|raw|disasm|disassembly|mnemonic|mnemonics|opcode|opcodes|instruction|instructions|code)"\s*:',
    re.IGNORECASE,
)
HEX_RUN = re.compile(r"(?<![0-9A-Fa-f])[0-9A-Fa-f]{32,}(?![0-9A-Fa-f])")
SPACED_HEX_BYTES = re.compile(r"(?:\b[0-9A-Fa-f]{2}[ ,]){16,}")
BASE64_RUN = re.compile(r"[A-Za-z0-9+/]{64,}={0,2}")
SHA256_FIELD = re.compile(r'"[a-z_]*sha256"\s*:\s*"[0-9a-f]{64}"')

IR_MARKERS = (b"target datalayout = ", b"target triple = ")


@dataclass
class Violation:
    path: str
    reason: str


def is_pe(data: bytes) -> bool:
    if len(data) < 0x40 or data[:2] != b"MZ":
        return False
    (offset,) = struct.unpack_from("<I", data, 0x3C)
    return offset + 4 <= len(data) and data[offset:offset + 4] == b"PE\0\0"


def binary_reason(data: bytes) -> Optional[str]:
    head = data[:8]
    if is_pe(data):
        return "Windows PE executable"
    if head[:4] in (b"BC\xc0\xde", b"\xde\xc0\x17\x0b"):
        return "LLVM bitcode"
    if head[:4] in (b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe"):
        return "Mach-O object"
    if head[:4] in (b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"):
        return "universal binary"
    if head[:4] == b"\x7fELF":
        return "ELF object"
    if head == b"!<arch>\n":
        return "static archive"
    if b"\ndefine " in data or data.startswith(b"define "):
        if any(marker in data for marker in IR_MARKERS):
            return "LLVM textual IR"
    return None


def analysis_reasons(text: str) -> List[str]:
    reasons = []
    for lineno, line in enumerate(text.splitlines(), 1):
        if ANALYSIS_FORBIDDEN_KEYS.search(line):
            reasons.append(f"line {lineno}: forbidden key (code bytes or disassembly are not allowed in analysis data)")
            continue
        scrubbed = SHA256_FIELD.sub('""', line)
        if HEX_RUN.search(scrubbed):
            reasons.append(f"line {lineno}: long hex run (looks like code bytes)")
        elif SPACED_HEX_BYTES.search(scrubbed):
            reasons.append(f"line {lineno}: byte sequence (looks like code bytes)")
        elif BASE64_RUN.search(scrubbed):
            reasons.append(f"line {lineno}: long base64 run (looks like an encoded blob)")
        if len(reasons) >= 5:
            reasons.append("further lines not reported")
            break
    return reasons


def load_allowlist(root: Path) -> set:
    path = root / "tools" / "repo-guard" / "allow-large.txt"
    if not path.exists():
        return set()
    entries = set()
    for line in path.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            entries.add(line)
    return entries


def check_file(rel: str, data: bytes, allow_large: set) -> List[Violation]:
    out = []
    norm = rel.replace(os.sep, "/")
    lower = norm.lower()
    if lower.startswith(DENIED_PREFIXES):
        out.append(Violation(norm, "game directory content"))
    ext = os.path.splitext(lower)[1]
    if ext in DENIED_EXTENSIONS:
        out.append(Violation(norm, f"denied file type {ext}"))
    reason = binary_reason(data)
    if reason:
        out.append(Violation(norm, reason))
    if len(data) > MAX_FILE_BYTES and norm not in allow_large:
        out.append(Violation(norm, f"file is {len(data)} bytes (limit {MAX_FILE_BYTES}; add to tools/repo-guard/allow-large.txt if intended)"))
    if lower.startswith(ANALYSIS_PREFIX):
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            out.append(Violation(norm, "analysis data must be UTF-8 text"))
        else:
            out.extend(Violation(norm, r) for r in analysis_reasons(text))
    return out


def git(root: Path, *args: str) -> bytes:
    return subprocess.run(["git", *args], cwd=root, check=True, stdout=subprocess.PIPE).stdout


def staged_files(root: Path) -> Iterable[Tuple[str, bytes]]:
    names = git(root, "diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z").split(b"\0")
    for raw in names:
        if raw:
            name = raw.decode()
            yield name, git(root, "show", f":{name}")


def tracked_files(root: Path) -> Iterable[Tuple[str, bytes]]:
    for raw in git(root, "ls-files", "-z").split(b"\0"):
        if raw:
            name = raw.decode()
            path = root / name
            if path.is_file():
                yield name, path.read_bytes()


def display_path(root: Path, base: Path, path: Path) -> str:
    resolved = path.resolve()
    try:
        return resolved.relative_to(root).as_posix()
    except ValueError:
        return resolved.relative_to(base.resolve()).as_posix() if base.is_dir() else path.name


def tree_files(root: Path, paths: List[Path]) -> Iterable[Tuple[str, bytes]]:
    for base in paths:
        if base.is_file():
            yield display_path(root, base, base), base.read_bytes()
            continue
        for dirpath, _, filenames in os.walk(base):
            for filename in sorted(filenames):
                path = Path(dirpath) / filename
                if path.is_file() and not path.is_symlink():
                    yield display_path(root, base, path), path.read_bytes()


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description="Reject game bytes, translated code and binaries from the repo or release artifacts.")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--staged", action="store_true", help="check files staged for commit")
    mode.add_argument("--tracked", action="store_true", help="check every tracked file")
    mode.add_argument("--paths", nargs="+", type=Path, help="check files or directory trees (release artifacts)")
    args = parser.parse_args(argv)

    root = Path(__file__).resolve().parents[2]
    allow_large = load_allowlist(root)
    if args.staged:
        files = staged_files(root)
    elif args.tracked:
        files = tracked_files(root)
    else:
        files = tree_files(root, args.paths)

    violations = []
    count = 0
    for rel, data in files:
        count += 1
        violations.extend(check_file(rel, data, allow_large))

    if violations:
        print("repo-guard: blocked", file=sys.stderr)
        for v in violations:
            print(f"  {v.path}: {v.reason}", file=sys.stderr)
        print("MacAndreas never stores game files, translated game code or build outputs. See ROADMAP.md §1.", file=sys.stderr)
        return 1
    print(f"repo-guard: {count} file(s) clean")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
