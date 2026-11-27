#!/usr/bin/env python3
import argparse
import bisect
import glob
import hashlib
import json
import re
import struct
import subprocess
import sys
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
CONFIG = HERE / "mods.json"
CACHE = ROOT / ".cache" / "mods"
MODS_DIR = ROOT / "mods"

DECISIONS = ("pending", "keep", "replace", "drop")
KEPT_VERSIONS = ("1.0", "any")


def load_config(path: Path = CONFIG) -> Dict[str, dict]:
    return json.loads(path.read_text())


@dataclass
class Source:
    code: str
    masked: str
    comments: Dict[int, str]
    line_starts: List[int]

    def line_of(self, pos: int) -> int:
        return bisect.bisect_right(self.line_starts, pos)


def _line_starts(text: str) -> List[int]:
    starts = [0]
    for i, c in enumerate(text):
        if c == "\n":
            starts.append(i + 1)
    return starts


def _digit_separator(text: str, i: int) -> bool:
    if i == 0 or i + 1 >= len(text) or not text[i - 1].isalnum() or not text[i + 1].isalnum():
        return False
    j = i - 1
    while j >= 0 and (text[j].isalnum() or text[j] in "'_"):
        j -= 1
    return text[j + 1].isdigit()


def clean(text: str) -> Tuple[str, str, Dict[int, str]]:
    code = []
    masked = []
    comments: Dict[int, str] = {}
    i = 0
    n = len(text)
    line = 1
    line_has_code = False
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = text.find("\n", i)
            if j < 0:
                j = n
            body = text[i + 2:j].strip()
            if not line_has_code and body:
                comments[line] = body
            code.append(" " * (j - i))
            masked.append(" " * (j - i))
            i = j
            continue
        if c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            chunk = text[i:j]
            blank = "".join("\n" if ch == "\n" else " " for ch in chunk)
            line += chunk.count("\n")
            if "\n" in chunk:
                line_has_code = False
            code.append(blank)
            masked.append(blank)
            i = j
            continue
        raw = re.match(r'(?:u8|u|U|L)?R"([^()\\\s]{0,16})\(', text[i:i + 24]) if c in 'RuUL' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")) else None
        if raw:
            end_tok = ")" + raw.group(1) + '"'
            j = text.find(end_tok, i + raw.end())
            j = n if j < 0 else j + len(end_tok)
            chunk = text[i:j]
            line += chunk.count("\n")
            code.append(chunk)
            masked.append('"' + "".join("\n" if ch == "\n" else " " for ch in chunk[2:-1]) + '"' if len(chunk) >= 2 else chunk)
            line_has_code = True
            i = j
            continue
        if c == '"' or (c == "'" and not _digit_separator(text, i)):
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                if text[j] == "\\":
                    j += 1
                j += 1
            closed = j < n and text[j] == c
            j = j + 1 if closed else j
            chunk = text[i:j]
            code.append(chunk)
            line += chunk.count("\n")
            inner = chunk[1:-1] if closed and len(chunk) >= 2 else chunk[1:]
            blank = "".join("\n" if ch == "\n" else " " for ch in inner)
            masked.append(c + blank + (c if closed and len(chunk) >= 2 else ""))
            line_has_code = True
            i = j
            continue
        if c == "\n":
            line += 1
            line_has_code = False
        elif not c.isspace():
            line_has_code = True
        code.append(c)
        masked.append(c)
        i += 1
    return "".join(code), "".join(masked), comments


_PP_TOKEN = re.compile(r"\s*(defined|[A-Za-z_]\w*|0[xX][0-9a-fA-F]+|\d+|&&|\|\||==|!=|<=|>=|[!()<>+\-])")


class _PPExpr:
    def __init__(self, text: str, defines: Dict[str, int]):
        self.tokens = []
        pos = 0
        text = re.sub(r"[uUlL]+\b", "", text)
        while pos < len(text):
            m = _PP_TOKEN.match(text, pos)
            if not m:
                if text[pos:].strip() == "":
                    break
                raise ValueError(text)
            self.tokens.append(m.group(1))
            pos = m.end()
        self.i = 0
        self.defines = defines

    def peek(self) -> Optional[str]:
        return self.tokens[self.i] if self.i < len(self.tokens) else None

    def take(self) -> str:
        t = self.tokens[self.i]
        self.i += 1
        return t

    def parse(self) -> int:
        v = self.or_()
        return v

    def or_(self) -> int:
        v = self.and_()
        while self.peek() == "||":
            self.take()
            r = self.and_()
            v = 1 if (v or r) else 0
        return v

    def and_(self) -> int:
        v = self.eq()
        while self.peek() == "&&":
            self.take()
            r = self.eq()
            v = 1 if (v and r) else 0
        return v

    def eq(self) -> int:
        v = self.rel()
        while self.peek() in ("==", "!="):
            op = self.take()
            r = self.rel()
            v = int(v == r) if op == "==" else int(v != r)
        return v

    def rel(self) -> int:
        v = self.add()
        while self.peek() in ("<", ">", "<=", ">="):
            op = self.take()
            r = self.add()
            v = int({"<": v < r, ">": v > r, "<=": v <= r, ">=": v >= r}[op])
        return v

    def add(self) -> int:
        v = self.unary()
        while self.peek() in ("+", "-"):
            op = self.take()
            r = self.unary()
            v = v + r if op == "+" else v - r
        return v

    def unary(self) -> int:
        t = self.peek()
        if t == "!":
            self.take()
            return int(not self.unary())
        if t == "-":
            self.take()
            return -self.unary()
        return self.primary()

    def primary(self) -> int:
        t = self.take()
        if t == "(":
            v = self.or_()
            if self.peek() == ")":
                self.take()
            return v
        if t == "defined":
            paren = self.peek() == "("
            if paren:
                self.take()
            name = self.take()
            if paren and self.peek() == ")":
                self.take()
            return int(name in self.defines)
        if t[0].isdigit():
            return int(t, 0)
        if self.peek() == "(":
            depth = 0
            while self.peek() is not None:
                x = self.take()
                if x == "(":
                    depth += 1
                elif x == ")":
                    depth -= 1
                    if depth == 0:
                        break
            return 0
        return self.defines.get(t, 0)


def pp_eval(text: str, defines: Dict[str, int]) -> bool:
    try:
        return bool(_PPExpr(text, defines).parse())
    except (ValueError, IndexError):
        return False


def preprocess(code: str, masked: str, defines: Sequence[str]) -> Tuple[str, str]:
    table = {d: 1 for d in defines}
    out_code = []
    out_masked = []
    stack: List[Tuple[bool, bool]] = []
    active = True
    code_lines = code.split("\n")
    masked_lines = masked.split("\n")
    continuing = False
    for cl, ml in zip(code_lines, masked_lines):
        stripped = cl.strip()
        directive = continuing or stripped.startswith("#")
        if directive:
            continuing = cl.rstrip().endswith("\\")
            m = re.match(r"#\s*(\w+)\s*(.*)", stripped) if stripped.startswith("#") else None
            if m:
                word, rest = m.group(1), m.group(2).rstrip("\\").strip()
                if word in ("if", "ifdef", "ifndef"):
                    if word == "ifdef":
                        cond = rest.split()[0] in table if rest else False
                    elif word == "ifndef":
                        cond = rest.split()[0] not in table if rest else True
                    else:
                        cond = pp_eval(rest, table)
                    stack.append((active, active and cond))
                    active = active and cond
                elif word == "elif" and stack:
                    parent, taken = stack[-1]
                    cond = parent and not taken and pp_eval(rest, table)
                    stack[-1] = (parent, taken or cond)
                    active = cond
                elif word == "else" and stack:
                    parent, taken = stack[-1]
                    active = parent and not taken
                    stack[-1] = (parent, True)
                elif word == "endif" and stack:
                    active = stack.pop()[0]
                elif word == "define" and active and rest:
                    name = re.match(r"[A-Za-z_]\w*", rest)
                    if name and "(" not in rest[:name.end() + 1]:
                        value = rest[name.end():].strip()
                        table[name.group(0)] = int(value, 0) if re.fullmatch(r"\d+|0[xX][0-9a-fA-F]+", value) else 1
                elif word == "undef" and active and rest:
                    table.pop(rest.split()[0], None)
            out_code.append(" " * len(cl))
            out_masked.append(" " * len(ml))
            continue
        if active:
            out_code.append(cl)
            out_masked.append(ml)
        else:
            out_code.append(" " * len(cl))
            out_masked.append(" " * len(ml))
    return "\n".join(out_code), "\n".join(out_masked)


def load_source(text: str, defines: Sequence[str]) -> Source:
    code, masked, comments = clean(text)
    code, masked = preprocess(code, masked, defines)
    return Source(code, masked, comments, _line_starts(code))


@dataclass
class Frame:
    kind: str
    name: str
    cond: str
    start: int
    end: int
    line: int
    depth: int
    params: List[str] = field(default_factory=list)
    paren: int = 0


CONTROL = ("if", "for", "while", "switch", "catch")
TRAILING_QUALIFIERS = re.compile(r"\s*(?:\b(?:const|noexcept|override|final|mutable|try|volatile)\b|->\s*[\w:<>*&\s,]+)\s*$")


def _matching_open(text: str, close: int, open_ch: str = "(", close_ch: str = ")") -> int:
    depth = 0
    for i in range(close, -1, -1):
        if text[i] == close_ch:
            depth += 1
        elif text[i] == open_ch:
            depth -= 1
            if depth == 0:
                return i
    return -1


def param_names(inner: str) -> List[str]:
    out = []
    for part in split_top(inner) if inner.strip() else []:
        part = part.split("=")[0].strip()
        m = re.search(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?$", part)
        out.append(m.group(1) if m and len(part.split()) > 1 else "")
    return out


def lambda_info(h: str) -> Tuple[str, List[str]]:
    m = re.search(r"([A-Za-z_]\w*)\s*(?:=|\()\s*\[[^\]]*\]\s*(?:\(([^()]*)\))?", h)
    if not m:
        m2 = re.search(r"\[[^\]]*\]\s*\(([^()]*)\)", h)
        return "", param_names(m2.group(1)) if m2 else []
    return m.group(1), param_names(m.group(2) or "")


def classify(header: str) -> Tuple[str, str, str]:
    h = " ".join(header.split())
    h = re.sub(r"^(?:\w+\s*:(?!:)\s*)+", "", h)
    if not h:
        return "block", "", ""
    if re.match(r"^(?:inline\s+)?namespace\b", h) or h.startswith('extern "'):
        m = re.match(r"^(?:inline\s+)?namespace\s+([\w:]+)", h)
        return "namespace", m.group(1) if m else "", ""
    if re.match(r"^(?:export\s+)?(?:template\s*<.*>\s*)?(?:class|struct|union|enum)\b", h) and "(" not in h.split("{")[0].split(":")[0]:
        m = re.match(r"^(?:export\s+)?(?:template\s*<.*>\s*)?(?:class|struct|union|enum)(?:\s+class)?\s+(?:\w+\s+)*?([\w:]+)\s*(?:final\s*)?(?::|$)", h)
        return "class", m.group(1) if m else "", ""
    if h in ("else", "do", "try") or h.endswith(" else") or h.endswith("=") or h.endswith(",") or h.endswith("return"):
        return "block", "", ""
    prev = None
    while prev != h:
        prev = h
        h = TRAILING_QUALIFIERS.sub("", h)
    if h.endswith("]"):
        return "lambda", lambda_info(h)[0], ""
    if not h.endswith(")"):
        return "block", "", ""
    open_pos = _matching_open(h, len(h) - 1)
    if open_pos < 0:
        return "block", "", ""
    before = h[:open_pos].rstrip()
    inner = h[open_pos + 1:-1].strip()
    if before.endswith("]"):
        return "lambda", lambda_info(h)[0], ""
    m = re.search(r"([~\w:]+(?:<[^<>]*>)?)\s*$", before)
    if not m:
        return "block", "", ""
    word = m.group(1)
    last = word.split("::")[-1]
    if last in CONTROL:
        return last, "", inner
    if before.endswith("else if") or last == "if":
        return "if", "", inner
    if "=" in before and not re.search(r"\boperator\b", before):
        return "block", "", ""
    if re.match(r"^(?:return|throw|new|case)\b", before):
        return "block", "", ""
    return "function", re.sub(r"<[^<>]*>$", "", word), ""


def frames_of(src: Source) -> List[Frame]:
    text = src.masked
    frames: List[Frame] = []
    stack: List[Frame] = []
    boundary = 0
    paren = 0
    for i, c in enumerate(text):
        if c == "(":
            paren += 1
        elif c == ")":
            paren = max(0, paren - 1)
        elif c in ";" and paren == 0:
            boundary = i + 1
        elif c == "{":
            header = text[boundary:i]
            kind, name, cond = classify(header)
            if paren > 0 and kind not in ("lambda",):
                kind = "block"
            f = Frame(kind, name, cond, i, len(text), src.line_of(i), len(stack))
            if kind == "lambda":
                f.params = lambda_info(" ".join(header.split()))[1]
            elif kind == "function":
                hh = TRAILING_QUALIFIERS.sub("", " ".join(header.split()))
                hh = TRAILING_QUALIFIERS.sub("", hh)
                op = _matching_open(hh, len(hh) - 1) if hh.endswith(")") else -1
                if op >= 0:
                    f.params = param_names(hh[op + 1:-1])
            f.paren = paren
            stack.append(f)
            frames.append(f)
            boundary = i + 1
            paren = 0
        elif c == "}":
            if stack:
                f = stack.pop()
                f.end = i
                paren = f.paren
            boundary = i + 1
    return frames


def frames_at(frames: List[Frame], pos: int) -> List[Frame]:
    return [f for f in frames if f.start < pos < f.end]


@dataclass
class Image:
    base: int
    sections: List[Tuple[int, int, int, int, int]]
    data: bytes
    code_start: int
    code_end: int
    mapped: Optional[bytearray] = None
    pattern_cache: Dict[str, List[int]] = field(default_factory=dict)

    @staticmethod
    def load(path: Path) -> "Image":
        data = path.read_bytes()
        if data[:2] != b"MZ":
            raise ValueError(f"{path} is not a PE file")
        (pe,) = struct.unpack_from("<I", data, 0x3C)
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError(f"{path} is not a PE file")
        nsect, = struct.unpack_from("<H", data, pe + 6)
        opt_size, = struct.unpack_from("<H", data, pe + 20)
        opt = pe + 24
        size_of_code, = struct.unpack_from("<I", data, opt + 4)
        base_of_code, = struct.unpack_from("<I", data, opt + 20)
        base, = struct.unpack_from("<I", data, opt + 28)
        sections = []
        table = opt + opt_size
        for k in range(nsect):
            off = table + 40 * k
            vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, off + 8)
            flags, = struct.unpack_from("<I", data, off + 36)
            sections.append((base + va, vsize, rawptr, rawsize, flags))
        scan_end = 0
        for va, vsize, rawptr, rawsize, flags in sections:
            if flags & 0x20000000:
                scan_end = va + (rawsize if rawsize else vsize)
        if scan_end == 0:
            scan_end = base + base_of_code + size_of_code
        return Image(base, sections, data, base, scan_end)

    def build_map(self) -> bytearray:
        if self.mapped is None:
            top = max(va + max(vs, rs) for va, vs, _, rs, _ in self.sections)
            m = bytearray(top - self.base)
            for va, vsize, rawptr, rawsize, _ in self.sections:
                n = min(rawsize, vsize if vsize else rawsize)
                m[va - self.base:va - self.base + n] = self.data[rawptr:rawptr + n]
            self.mapped = m
        return self.mapped

    def contains(self, addr: int) -> bool:
        return any(va <= addr < va + max(vs, rs) for va, vs, _, rs, _ in self.sections)

    def read(self, addr: int, size: int) -> Optional[bytes]:
        for va, vsize, rawptr, rawsize, _ in self.sections:
            if va <= addr and addr + size <= va + min(rawsize, vsize if vsize else rawsize):
                off = rawptr + addr - va
                return self.data[off:off + size]
        return None

    def u32(self, addr: int) -> Optional[int]:
        b = self.read(addr, 4)
        return struct.unpack("<I", b)[0] if b else None

    def scan(self, pattern: str, lo_addr: Optional[int] = None, hi_addr: Optional[int] = None) -> List[int]:
        key = " ".join(pattern.split())
        cache_key = key if lo_addr is None else f"{key}@{lo_addr:x}-{hi_addr:x}"
        if cache_key in self.pattern_cache:
            return self.pattern_cache[cache_key]
        parts = []
        for tok in key.split(" "):
            if not tok:
                continue
            if set(tok) <= {"?"}:
                parts.append(b".")
                continue
            if len(tok) % 2:
                raise ValueError(f"bad pattern token {tok!r}")
            for k in range(0, len(tok), 2):
                parts.append(re.escape(bytes([int(tok[k:k + 2], 16)])))
        rx = re.compile(b"(?=(" + b"".join(parts) + b"))", re.DOTALL)
        mapped = self.build_map()
        if lo_addr is None:
            lo = self.code_start - self.base
            hi = min(self.code_end - self.base, len(mapped))
        else:
            lo = max(lo_addr - self.base, 0)
            hi = min(hi_addr - self.base, len(mapped))
        hits = [self.base + lo + m.start() for m in rx.finditer(bytes(mapped[lo:hi]))]
        self.pattern_cache[cache_key] = hits
        return hits

    def branch_target(self, addr: int) -> Optional[int]:
        b = self.read(addr, 6)
        if not b:
            return None
        if b[0] in (0xE8, 0xE9):
            return (addr + 5 + struct.unpack_from("<i", b, 1)[0]) & 0xFFFFFFFF
        if b[0] == 0xEB or 0x70 <= b[0] <= 0x7F:
            return (addr + 2 + struct.unpack_from("<b", b, 1)[0]) & 0xFFFFFFFF
        if b[0] == 0x0F and 0x80 <= b[1] <= 0x8F:
            return (addr + 6 + struct.unpack_from("<i", b, 2)[0]) & 0xFFFFFFFF
        if b[0] == 0xFF and b[1] == 0x15:
            return self.u32(addr + 2)
        return None


@dataclass
class Value:
    kind: str
    addrs: List[int] = field(default_factory=list)
    pins: List[str] = field(default_factory=list)
    notes: List[str] = field(default_factory=list)
    version: str = ""
    expect: Optional[int] = None
    reason: str = ""

    def with_pin(self, pin: str) -> "Value":
        if pin not in self.pins:
            self.pins.append(pin)
        return self


def unknown(reason: str) -> Value:
    return Value("unknown", reason=reason)


def split_top(text: str, seps: str = ",") -> List[str]:
    out = []
    depth = 0
    cur = []
    i = 0
    while i < len(text):
        c = text[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < len(text) and text[j] != c:
                if text[j] == "\\":
                    j += 1
                j += 1
            cur.append(text[i:j + 1])
            i = j + 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "<" and _template_open(text, i):
            close = _template_close(text, i)
            if close > 0:
                cur.append(text[i:close + 1])
                i = close + 1
                continue
        if c in seps and depth == 0:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    out.append("".join(cur))
    return [s.strip() for s in out]


def _template_open(text: str, i: int) -> bool:
    if i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_"):
        return False
    j = i - 1
    while j >= 0 and (text[j].isalnum() or text[j] in "_:"):
        j -= 1
    word = text[j + 1:i]
    return bool(word) and not word[0].isdigit()


def _template_close(text: str, i: int) -> int:
    depth = 0
    for k in range(i, len(text)):
        c = text[k]
        if c == "<":
            depth += 1
        elif c == ">":
            depth -= 1
            if depth == 0:
                return k
        elif c in ";{}":
            return -1
    return -1


def strip_parens(text: str) -> str:
    t = text.strip()
    while t.startswith("(") and t.endswith(")") and _matching_open(t, len(t) - 1) == 0:
        t = t[1:-1].strip()
    return t


CAST_TYPES = r"(?:const\s+|volatile\s+|unsigned\s+|signed\s+|struct\s+|class\s+)*(?:[A-Za-z_][\w:]*)(?:\s*<[^()]*>)?(?:\s*(?:\*|&|const\b|\(\s*\*\s*\)\s*\([^()]*\)))*"
C_CAST = re.compile(r"^\(\s*(" + CAST_TYPES + r")\s*\)\s*(?=[\w(*&\[{\"]|$)")
NAMED_CAST = re.compile(r"^(?:reinterpret_cast|static_cast|const_cast|std::bit_cast)\s*<")
SCALAR_TYPES = {"void", "char", "short", "int", "long", "float", "double", "bool", "BYTE", "WORD", "DWORD", "uintptr_t",
                "intptr_t", "size_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t",
                "int64_t", "unsigned", "signed", "auto", "LPVOID", "PVOID", "ptrdiff_t", "decltype"}

TYPE_SIZES = {"BYTE": 1, "uint8_t": 1, "int8_t": 1, "char": 1, "bool": 1, "unsigned char": 1, "signed char": 1,
              "WORD": 2, "uint16_t": 2, "int16_t": 2, "short": 2, "unsigned short": 2,
              "DWORD": 4, "uint32_t": 4, "int32_t": 4, "int": 4, "unsigned int": 4, "float": 4, "uintptr_t": 4,
              "intptr_t": 4, "size_t": 4, "long": 4, "unsigned long": 4, "LONG": 4, "UINT": 4,
              "double": 8, "uint64_t": 8, "int64_t": 8, "long long": 8}


def type_size(t: str) -> Optional[int]:
    t = " ".join(t.replace("const", " ").split())
    if not t:
        return None
    if t.endswith("*") or t.endswith("&") or "(*)" in t or t.startswith("decltype"):
        return 4
    return TYPE_SIZES.get(t)


def is_cast_type(t: str) -> bool:
    t = t.strip()
    base = re.sub(r"<.*>", "", t).replace("const", "").replace("volatile", "").strip()
    if t.endswith("*") or t.endswith("&") or "(*)" in t:
        return True
    first = base.split()[0] if base.split() else ""
    return first in SCALAR_TYPES or base in SCALAR_TYPES


class Evaluator:
    def __init__(self, src: Source, frames: List[Frame], image: Optional[Image]):
        self.src = src
        self.frames = frames
        self.image = image
        self.depth = 0

    def eval(self, text: str, pos: int) -> Value:
        self.depth += 1
        try:
            if self.depth > 24:
                return unknown("expression too deep")
            return self._eval(text, pos)
        finally:
            self.depth -= 1

    def _eval(self, text: str, pos: int) -> Value:
        t = strip_parens(text)
        if not t:
            return unknown("empty expression")
        m = NAMED_CAST.match(t)
        if m:
            close = _template_close(t, m.end() - 1)
            if close > 0:
                rest = t[close + 1:].strip()
                if rest.startswith("("):
                    end = self._close_paren(rest, 0)
                    if end == len(rest) - 1:
                        return self.eval(rest[1:-1], pos)
                    return self._postfix(self.eval(rest[1:end], pos), rest[end + 1:], pos)
        m = C_CAST.match(t)
        if m and is_cast_type(m.group(1)):
            return self.eval(t[m.end():], pos)
        parts = self._split_additive(t)
        if len(parts) > 1:
            total = self.eval(parts[0][1], pos)
            if parts[0][0] == "-":
                return unknown("leading minus")
            for sign, term in parts[1:]:
                rhs = self.eval(term, pos)
                if rhs.kind != "addr" or len(rhs.addrs) != 1:
                    return unknown(f"non-constant offset in {self.redact(t)}")
                if total.kind != "addr":
                    return total
                delta = rhs.addrs[0] if sign == "+" else -rhs.addrs[0]
                total = Value("addr", [(a + delta) & 0xFFFFFFFF for a in total.addrs], total.pins + [p for p in rhs.pins if p not in total.pins and p != "literal"],
                              total.notes + rhs.notes, total.version or rhs.version, total.expect)
            return total
        lit = re.fullmatch(r"(0[xX][0-9a-fA-F]+|\d+)[uUlL]*", t)
        if lit:
            return Value("addr", [int(lit.group(1), 0)], ["literal"])
        if t.startswith("*"):
            inner = self.eval(t[1:], pos)
            if inner.kind != "addr":
                return inner if inner.kind == "unknown" else unknown("dereference of non-address")
            if not self.image:
                return unknown("dereference needs --exe")
            vals = [self.image.u32(a) for a in inner.addrs]
            if any(v is None for v in vals):
                return unknown("dereference outside file data")
            return Value("addr", vals, inner.pins + ["deref"], inner.notes, inner.version)
        if t.startswith("&") or t.startswith("{") or t.startswith('"') or t.startswith("["):
            return unknown(f"not a game address: {self.redact(t)[:40]}")
        return self._chain(t, pos)

    @staticmethod
    def _close_paren(text: str, start: int, open_ch: str = "(", close_ch: str = ")") -> int:
        depth = 0
        i = start
        while i < len(text):
            c = text[i]
            if c == '"':
                j = i + 1
                while j < len(text) and text[j] != '"':
                    if text[j] == "\\":
                        j += 1
                    j += 1
                i = j + 1
                continue
            if c == open_ch:
                depth += 1
            elif c == close_ch:
                depth -= 1
                if depth == 0:
                    return i
            i += 1
        return -1

    def _split_additive(self, t: str) -> List[Tuple[str, str]]:
        parts = []
        depth = 0
        cur_start = 0
        sign = "+"
        i = 0
        while i < len(t):
            c = t[i]
            if c == '"':
                j = i + 1
                while j < len(t) and t[j] != '"':
                    if t[j] == "\\":
                        j += 1
                    j += 1
                i = j + 1
                continue
            if c in "([{":
                depth += 1
            elif c in ")]}":
                depth -= 1
            elif c == "<" and _template_open(t, i):
                close = _template_close(t, i)
                if close > 0:
                    i = close + 1
                    continue
            elif c in "+-" and depth == 0 and i > 0:
                prev = t[:i].rstrip()
                if prev and (prev[-1].isalnum() or prev[-1] in ")]_") and not (c == "-" and i + 1 < len(t) and t[i + 1] == ">") and not (i + 1 < len(t) and t[i + 1] == c):
                    parts.append((sign, t[cur_start:i].strip()))
                    sign = c
                    cur_start = i + 1
            i += 1
        parts.append((sign, t[cur_start:].strip()))
        return parts

    def _chain(self, t: str, pos: int) -> Value:
        m = re.match(r"([A-Za-z_][\w]*(?:::[A-Za-z_~]\w*)*)", t)
        if not m:
            return unknown(f"unsupported expression {self.redact(t)[:40]}")
        name = m.group(1)
        rest = t[m.end():].lstrip()
        targs = ""
        if rest.startswith("<"):
            close = _template_close(rest, 0)
            if close > 0:
                targs = rest[1:close]
                rest = rest[close + 1:].lstrip()
        if rest.startswith("("):
            end = self._close_paren(rest, 0)
            if end < 0:
                return unknown("unbalanced call")
            args = split_top(rest[1:end]) if rest[1:end].strip() else []
            val = self._call(name, targs, args, pos)
            return self._postfix(val, rest[end + 1:], pos)
        if name in ("nullptr", "NULL", "true", "false"):
            return unknown(f"not a game address: {name}")
        val = self.lookup(name, pos)
        return self._postfix(val, rest, pos)

    def _postfix(self, val: Value, rest: str, pos: int) -> Value:
        rest = rest.strip()
        while rest:
            m = re.match(r"(?:\.|->)\s*([A-Za-z_]\w*)\s*", rest)
            if not m:
                if rest.startswith("["):
                    return unknown("subscript")
                return unknown(f"unsupported suffix {self.redact(rest)[:30]}")
            meth = m.group(1)
            rest = rest[m.end():]
            if rest.startswith("<"):
                close = _template_close(rest, 0)
                rest = rest[close + 1:].lstrip() if close > 0 else rest
            args: List[str] = []
            called = False
            if rest.startswith("("):
                end = self._close_paren(rest, 0)
                if end < 0:
                    return unknown("unbalanced call")
                inner = rest[1:end]
                args = split_top(inner) if inner.strip() else []
                rest = rest[end + 1:].lstrip()
                called = True
            val = self._method(val, meth, args, called, pos)
            if val.kind == "unknown":
                return val
        return val

    def _int_arg(self, args: List[str], k: int, pos: int, default: int = 0) -> Optional[int]:
        if k >= len(args):
            return default
        v = self.eval(args[k], pos)
        if v.kind == "addr" and len(v.addrs) == 1:
            return v.addrs[0] if v.addrs[0] < 0x80000000 else v.addrs[0] - 0x100000000
        return None

    def _string_arg(self, args: List[str]) -> Optional[str]:
        for a in args:
            m = re.fullmatch(r'\s*"((?:[^"\\]|\\.)*)"\s*', a)
            if m:
                return m.group(1)
            m = re.fullmatch(r"\s*std::string_view\s*\(\s*\"((?:[^\"\\]|\\.)*)\"\s*\)\s*", a)
            if m:
                return m.group(1)
        return None

    def _pattern(self, pattern: Optional[str], window: Optional[Tuple[int, int]] = None) -> Value:
        if pattern is None:
            return unknown("pattern text is not a literal")
        if not self.image:
            return unknown("pattern needs --exe")
        try:
            hits = self.image.scan(pattern, *window) if window else self.image.scan(pattern)
        except ValueError as e:
            return unknown(str(e))
        return Value("pattern", hits, ["pattern", "range"] if window else ["pattern"])

    def _range_arg(self, arg: str, pos: int) -> Optional[Tuple[int, int]]:
        inner = arg.strip()
        while inner.startswith("{") and inner.endswith("}"):
            inner = inner[1:-1].strip()
        parts = split_top(inner)
        if len(parts) != 2:
            return None
        lo, hi = self.eval(parts[0], pos), self.eval(parts[1], pos)
        if lo.kind != "addr" or hi.kind != "addr" or len(lo.addrs) != 1 or len(hi.addrs) != 1:
            return None
        return lo.addrs[0], hi.addrs[0]

    def _first(self, val: Value, off: int, require_one: bool = False) -> Value:
        if not val.addrs:
            return unknown("pattern not found")
        notes = list(val.notes)
        if len(val.addrs) != 1:
            notes.append(f"pattern matched {len(val.addrs)} times")
        if val.expect is not None and len(val.addrs) != val.expect:
            notes.append(f"pattern expected {val.expect} matches, found {len(val.addrs)}")
        return Value("addr", [(val.addrs[0] + off) & 0xFFFFFFFF], val.pins, notes, val.version)

    def _call(self, name: str, targs: str, args: List[str], pos: int) -> Value:
        short = name.split("::")[-1]
        if short in ("AddressByRegion_10", "DynBaseAddress", "AdjustAddress_10"):
            if not args:
                return unknown("no address")
            v = self.eval(args[0], pos)
            return v.with_pin("region_10") if v.kind == "addr" else v
        if short in ("AddressByRegion_11", "AdjustAddress_11"):
            v = self.eval(args[0], pos) if args else unknown("no address")
            v.version = "1.01"
            return v
        if short == "AddressByVersion":
            if not args:
                return unknown("no address")
            v = self.eval(args[0], pos)
            if v.kind == "addr":
                v.with_pin("version_10")
            return v
        if name in ("hook::pattern", "hook::module_pattern", "hook::make_module_pattern", "hook::range_pattern", "pattern", "hook::txn::pattern"):
            return self._pattern(self._string_arg(args))
        if short in ("get_pattern",) or name in ("Memory::PatternAndOffset", "PatternAndOffset"):
            window = None
            if args and args[0].strip().startswith("{"):
                window = self._range_arg(args[0], pos)
                if window is None:
                    return unknown("pattern range is not constant")
                args = args[1:]
            pattern = self._string_arg(args[:1])
            val = self._pattern(pattern, window)
            if val.kind != "pattern":
                return val
            off = self._int_arg(args, 1, pos)
            if off is None:
                return unknown("non-constant pattern offset")
            return self._first(val, off)
        if short == "GetBranchDestination" or short == "ReadRelativeOffset":
            if not args:
                return unknown("no address")
            inner = self.eval(args[0], pos)
            if inner.kind != "addr":
                return inner
            if not self.image:
                return unknown("branch destination needs --exe")
            targets = [self.image.branch_target(a) for a in inner.addrs]
            if any(x is None for x in targets):
                return unknown("no branch at address")
            return Value("addr", targets, inner.pins + ["branch"], inner.notes, inner.version)
        if short == "GetFunctionAddrIfRerouted" and args:
            v = self.eval(args[0], pos)
            if v.kind != "addr":
                return v
            if not self.image:
                return v.with_pin("reroute_check")
            followed = []
            for a in v.addrs:
                b = self.image.read(a, 1)
                t = self.image.branch_target(a) if b and b[0] == 0xE9 else None
                if t is not None and self.image.contains(t):
                    v.notes.append(f"0x{a:08x} jumps to 0x{t:08x}; the mod follows it")
                    followed.append(t)
                else:
                    followed.append(a)
            return Value("addr", followed, v.pins + ["reroute"], v.notes, v.version)
        if short in ("ReadMemory",) and args:
            inner = self.eval(args[0], pos)
            if inner.kind != "addr":
                return inner
            if not self.image:
                return unknown("memory read needs --exe")
            vals = [self.image.u32(a) for a in inner.addrs]
            if any(v is None for v in vals):
                return unknown("read outside file data")
            return Value("addr", vals, inner.pins + ["deref"], inner.notes, inner.version)
        if short in SCALAR_TYPES or short in ("memory_pointer", "memory_pointer_raw", "auto_pointer", "raw_ptr"):
            if len(args) == 1:
                return self.eval(args[0], pos)
        return unknown(f"call to {name}")

    def _method(self, val: Value, meth: str, args: List[str], called: bool, pos: int) -> Value:
        if val.kind == "pattern":
            if meth in ("count", "count_hint"):
                n = self._int_arg(args, 0, pos)
                if meth == "count":
                    val.expect = n
                return val
            if meth == "get_first":
                off = self._int_arg(args, 0, pos)
                if off is None:
                    return unknown("non-constant pattern offset")
                return self._first(val, off)
            if meth == "get_one":
                return self._first(val, 0, True)
            if meth == "get":
                if not args:
                    return unknown("get() without index")
                idx = self._int_arg(args, 0, pos)
                if idx is None:
                    notes = list(val.notes)
                    if val.expect is not None and len(val.addrs) != val.expect:
                        notes.append(f"pattern expected {val.expect} matches, found {len(val.addrs)}")
                    if not val.addrs:
                        return unknown("pattern not found")
                    return Value("addr", list(val.addrs), val.pins + ["multi"], notes, val.version)
                if idx >= len(val.addrs):
                    return unknown(f"pattern has {len(val.addrs)} matches, index {idx}")
                notes = list(val.notes)
                if val.expect is not None and len(val.addrs) != val.expect:
                    notes.append(f"pattern expected {val.expect} matches, found {len(val.addrs)}")
                return Value("addr", [val.addrs[idx]], val.pins, notes, val.version)
            if meth in ("size", "empty", "clear", "matches", "for_each_result"):
                return unknown(f"pattern {meth}()")
            return unknown(f"pattern method {meth}")
        if val.kind == "addr":
            if meth in ("get", "get_raw", "as_int", "as_ptr", "address", "getx", "get_uintptr"):
                if args:
                    off = self._int_arg(args, 0, pos)
                    if off is None:
                        return unknown("non-constant offset")
                    return Value("addr", [(a + off) & 0xFFFFFFFF for a in val.addrs], val.pins, val.notes, val.version)
                return val
            if meth == "get_first":
                return val
            return unknown(f"method {meth} on address")
        return val

    def lookup(self, name: str, pos: int) -> Value:
        if not re.fullmatch(r"[A-Za-z_]\w*", name):
            return unknown(f"unknown name {name}")
        here = frames_at(self.frames, pos)
        here_ids = {id(f) for f in here}
        rx = re.compile(r"(?<![\w.>:])" + re.escape(name) + r"\s*=(?!=)")
        masked = self.src.masked
        best = None
        for m in rx.finditer(masked, 0, pos):
            if masked[m.start() - 1:m.start()] in ("!", "<", ">"):
                continue
            ids = {id(f) for f in frames_at(self.frames, m.start())}
            if ids <= here_ids:
                best = m
        ranged = re.compile(r"\bfor\s*\(\s*(?:const\s+)?(?:auto|[\w:]+)\s*&{0,2}\s*" + re.escape(name) + r"\s*:\s*\{([^{}]*)\}\s*\)")
        owner_start = next((f.start for f in reversed(here) if f.kind in ("function", "lambda")), 0)
        last = None
        for m in ranged.finditer(self.src.code, owner_start, pos):
            last = m
        if last is not None and (best is None or last.start() > best.start()):
            addrs: List[int] = []
            for item in split_top(last.group(1)):
                if not item:
                    continue
                v = self.eval(item, last.start())
                if v.kind != "addr":
                    return unknown(f"list element of {name}: {v.reason or v.kind}")
                addrs.extend(v.addrs)
            return Value("addr", addrs, ["literal", "multi"])
        if best is None:
            via = self._via_callers(name, here, pos)
            if via is not None:
                return via
            loop = re.compile(r"\bfor\s*\([^;]*\b" + re.escape(name) + r"\b")
            for f in here:
                if f.kind == "for" and loop.search(masked, max(0, f.start - 400), f.start):
                    return unknown(f"loop variable {name}")
            return unknown(f"unresolved name {name}")
        seg_start = max(masked.rfind(";", 0, best.start()), masked.rfind("{", 0, best.start()), masked.rfind("}", 0, best.start()))
        if re.search(r"\bfor\s*\([\w:\s<>*&]*$", masked[seg_start + 1:best.start()]):
            return unknown(f"loop variable {name}")
        start = best.end()
        end = self._statement_end(start)
        expr = self.src.code[start:end]
        return self.eval(expr, best.start())

    def _via_callers(self, name: str, here: List[Frame], pos: int) -> Optional[Value]:
        owner = None
        for f in reversed(here):
            if f.kind in ("function", "lambda"):
                owner = f
                break
        if owner is None or name not in owner.params or not owner.name:
            return None
        if self.depth > 12:
            return unknown(f"parameter {name} of {owner.name} nests too deeply")
        index = owner.params.index(name)
        short = owner.name.split("::")[-1]
        rx = re.compile(r"(?<![\w.>])(?:[\w:]+::)?" + re.escape(short) + r"\s*(?:<[^<>;]*>)?\s*\(")
        addrs: List[int] = []
        pins: List[str] = []
        notes: List[str] = []
        callers = 0
        for m in rx.finditer(self.src.masked):
            if owner.start - 400 < m.start() < owner.start:
                continue
            if owner.start < m.start() < owner.end:
                continue
            open_pos = m.end() - 1
            end = self._close_paren(self.src.masked, open_pos)
            if end < 0:
                continue
            args = split_top(self.src.code[open_pos + 1:end]) if self.src.code[open_pos + 1:end].strip() else []
            if index >= len(args):
                continue
            callers += 1
            v = self.eval(args[index], m.start())
            if v.kind != "addr":
                return unknown(f"parameter {name} of {short}: {v.reason or v.kind}")
            addrs.extend(v.addrs)
            pins.extend(p for p in v.pins if p not in pins)
            notes.extend(v.notes)
        if not callers:
            return unknown(f"parameter {name} of {short} has no callers in this file")
        return Value("addr", addrs, pins + [f"via:{short}"], notes)

    def _statement_end(self, start: int) -> int:
        masked = self.src.masked
        depth = 0
        for i in range(start, len(masked)):
            c = masked[i]
            if c in "([{":
                depth += 1
            elif c in ")]}":
                if depth == 0:
                    return i
                depth -= 1
            elif (c == ";" or c == ",") and depth == 0:
                return i
        return len(masked)

    @staticmethod
    def redact(text: str) -> str:
        return " ".join(re.sub(r'"(?:[^"\\]|\\.)*"', '"…"', text).split())


@dataclass
class OpSpec:
    op: str
    addr: int = 0
    value: Optional[int] = None
    target: Optional[int] = None
    count: Optional[int] = None
    end: Optional[int] = None
    span: Optional[int] = None
    writes: bool = True


OPS: Dict[str, OpSpec] = {
    "Patch": OpSpec("patch", value=1),
    "Nop": OpSpec("nop", count=1),
    "InjectHook": OpSpec("inject_hook", target=1, span=5),
    "InterceptCall": OpSpec("intercept_call", target=2, span=5),
    "ReadCall": OpSpec("read_call", target=1, span=5, writes=False),
    "Read": OpSpec("read", writes=False),
    "MemEquals": OpSpec("verify", writes=False),
    "Verify": OpSpec("verify", writes=False),
    "WriteOffsetValue": OpSpec("write_offset", value=1),
    "ReadOffsetValue": OpSpec("read_offset", writes=False),
    "WriteMemDisplacement": OpSpec("write_displacement", value=1),
    "ReadMemDisplacement": OpSpec("read_displacement", writes=False),
    "InterceptMemDisplacement": OpSpec("intercept_displacement", target=2),
    "injector::WriteMemory": OpSpec("patch", value=1),
    "injector::WriteObject": OpSpec("patch", value=1),
    "injector::WriteMemoryRaw": OpSpec("patch_raw", value=1, count=2),
    "injector::MemoryFill": OpSpec("fill", value=1, count=2),
    "injector::MakeNOP": OpSpec("nop", count=1),
    "injector::MakeRangedNOP": OpSpec("nop_range", end=1),
    "injector::MakeCALL": OpSpec("make_call", target=1, span=5),
    "injector::MakeJMP": OpSpec("make_jmp", target=1, span=5),
    "injector::MakeJA": OpSpec("make_ja", target=1, span=6),
    "injector::MakeRET": OpSpec("make_ret", value=1),
    "injector::MakeInline": OpSpec("make_inline", end=1),
    "injector::MakeRelativeOffset": OpSpec("write_relative", target=1, span=4),
    "injector::AdjustPointer": OpSpec("adjust_pointer", span=4),
    "injector::ReadMemory": OpSpec("read", writes=False),
    "injector::ReadMemoryRaw": OpSpec("read", writes=False),
    "injector::ReadObject": OpSpec("read", writes=False),
    "safetyhook::create_mid": OpSpec("mid_hook", target=1, span=5),
    "safetyhook::create_inline": OpSpec("inline_hook", target=1, span=5),
}

OP_ALIASES = {
    "Memory::Patch": "Patch", "Memory::VP::Patch": "Patch", "Memory::DynBase::Patch": "Patch",
    "Memory::Nop": "Nop", "Memory::VP::Nop": "Nop", "Memory::DynBase::Nop": "Nop",
    "Memory::InjectHook": "InjectHook", "Memory::VP::InjectHook": "InjectHook", "Memory::DynBase::InjectHook": "InjectHook",
    "Memory::ReadCall": "ReadCall", "Memory::VP::ReadCall": "ReadCall", "Memory::InterceptCall": "InterceptCall",
    "Memory::VP::InterceptCall": "InterceptCall", "Memory::Read": "Read", "Memory::VP::Read": "Read",
    "Memory::WriteOffsetValue": "WriteOffsetValue", "Memory::ReadOffsetValue": "ReadOffsetValue",
    "Memory::VP::WriteOffsetValue": "WriteOffsetValue", "Memory::VP::ReadOffsetValue": "ReadOffsetValue",
    "Memory::InterceptMemDisplacement": "InterceptMemDisplacement", "Memory::VP::InterceptMemDisplacement": "InterceptMemDisplacement",
    "Memory::WriteMemDisplacement": "WriteMemDisplacement", "Memory::ReadMemDisplacement": "ReadMemDisplacement",
    "Memory::VP::WriteMemDisplacement": "WriteMemDisplacement", "Memory::VP::ReadMemDisplacement": "ReadMemDisplacement",
    "Memory::MemEquals": "MemEquals", "Memory::Verify": "Verify",
}

CALL_RX = re.compile(r"(?<![\w.>])((?:[A-Za-z_]\w*::)*[A-Za-z_]\w*)\s*(?=[<(])")
BIND_RX = re.compile(r"(?<![\w.>])(?:Memory::)?(AddressByVersion|External(?:Ref|Func|Method|Var)\w*)\b")
DEREF_RX = re.compile(r"\*\s*\(\s*" + CAST_TYPES + r"\s*\)\s*\(?\s*(0[xX][0-9a-fA-F]{6,8})\b")


@dataclass
class Site:
    op: str
    file: str
    line: int
    scope: str
    group: str
    cond: str
    expr: str
    value: str
    target: str
    size: Optional[int]
    result: Value
    version: str
    pos: int


def op_name(raw: str) -> Optional[str]:
    if raw in OPS:
        return raw
    if raw in OP_ALIASES:
        return OP_ALIASES[raw]
    return None


ASM_COMMENT = re.compile(r"^(?:push|pop|mov[a-z]*|xor|nop|test|jmp|j[a-z]{1,3}|call|add|sub|lea|retn?|cmp|inc|dec|and|or|shl|shr|sar|imul|f(?:ld|st|stp|add|sub|mul|div|comp?|xch|chs|abs|sqrt|ild|istp?|nstsw|ldz|ld1|cos|sin)|cdq|cwde|set[a-z]{1,3}|int3|leave)\b(?:\s+(?:e?[abcd]x|e?[sd]i|e?[sb]p|[abcd][lh]|byte|word|dword|qword|ptr|offset|loc_|sub_|st\(?\d|\[|0x|\d)|\s*$|\s*[\\/,])", re.I)
NOTE_COMMENT = re.compile(r"^(?:only\b|not in\b|by\s|contributed\b|thanks\b|todo\b|fixme\b|hack\b|see\s|ret\b|eax\b|ecx\b)", re.I)


def _heading_ok(c: str) -> bool:
    if re.search(r"[;{}]\s*$", c) or re.match(r"^\s*(?:[\w:]+\s*(?:<[^>]*>)?\s*\(|#|[*=/-]{3,})", c):
        return False
    if re.search(r"\b0x[0-9A-Fa-f]{5,}\b.*[;)]", c):
        return False
    c = c.strip(" /*-=")
    return len(c) >= 3 and not ASM_COMMENT.match(c) and not NOTE_COMMENT.match(c)


def heading_for(src: Source, frames: List[Frame], line: int, pos: int) -> str:
    here = frames_at(frames, pos)
    limit = 0
    for f in reversed(here):
        if f.kind in ("function", "lambda"):
            limit = f.line
            break
    ln = line
    floor = max(limit, line - 400)
    while ln >= floor:
        c = src.comments.get(ln)
        if not c:
            ln -= 1
            continue
        top = ln
        while top - 1 >= floor and src.comments.get(top - 1):
            top -= 1
        for k in range(top, ln + 1):
            if _heading_ok(src.comments[k]):
                return src.comments[k].strip(" /*-=")[:80]
        ln = top - 1
    return ""


def scope_name(frames: List[Frame]) -> str:
    names = []
    for f in frames:
        if f.kind in ("namespace", "class", "function") and f.name:
            names.append(f.name)
    return "::".join(names)


def cond_text(frames: List[Frame]) -> str:
    for f in reversed(frames):
        if f.kind == "if" and f.cond:
            return Evaluator.redact(f.cond)[:80]
        if f.kind in ("function",):
            break
    return ""


def version_for(cfg: dict, frames: List[Frame], value: Value) -> str:
    if value.version:
        return value.version
    for f in reversed(frames):
        if f.kind == "function" and f.name:
            short = f.name.split("::")[-1]
            for rule in cfg.get("versions", []):
                if re.search(rule["scope"], short):
                    return rule["version"]
    return "any"


def scan_text(cfg: dict, rel: str, text: str, image: Optional[Image]) -> List[Site]:
    src = load_source(text, cfg.get("defines", []))
    frames = frames_of(src)
    ev = Evaluator(src, frames, image)
    sites: List[Site] = []
    consumed: List[Tuple[int, int]] = []
    code = src.code
    for m in CALL_RX.finditer(src.masked):
        name = op_name(m.group(1))
        if not name:
            continue
        spec = OPS[name]
        p = m.end()
        targs = ""
        if src.masked[p] == "<":
            close = _template_close(src.masked, p)
            if close < 0:
                continue
            targs = code[p + 1:close]
            p = close + 1
            while p < len(code) and code[p].isspace():
                p += 1
        if p >= len(code) or code[p] != "(":
            continue
        end = Evaluator._close_paren(src.masked, p)
        if end < 0:
            continue
        before = src.masked[max(0, m.start() - 40):m.start()]
        if re.search(r"\b(?:void|inline|auto|bool|BOOL|static)\s*$", before) and not re.search(r"=\s*$", before):
            continue
        args = split_top(code[p + 1:end]) if code[p + 1:end].strip() else []
        consumed.append((p, end))
        here = frames_at(frames, m.start())
        addr_expr = args[spec.addr] if len(args) > spec.addr else ""
        if name == "injector::MakeInline" and targs:
            targ_parts = split_top(targs)
            if targ_parts and re.fullmatch(r"0[xX][0-9a-fA-F]+", targ_parts[0].strip()):
                addr_expr = targ_parts[0]
                if len(targ_parts) > 1:
                    args = [addr_expr, targ_parts[1]] + args
        result = ev.eval(addr_expr, m.start()) if addr_expr else unknown("no address argument")
        size = spec.span
        if spec.op == "patch":
            if targs:
                size = type_size(targs.split(",")[0])
            elif len(args) > 1 and args[1].startswith("{"):
                size = len([x for x in split_top(args[1].strip()[1:-1]) if x])
            elif len(args) > 1 and (args[1].startswith("&") or "nullptr" in args[1]):
                size = 4
        if spec.count is not None and len(args) > spec.count:
            cnt = ev.eval(args[spec.count], m.start())
            if cnt.kind == "addr" and len(cnt.addrs) == 1 and cnt.addrs[0] < 0x10000:
                size = cnt.addrs[0]
        if spec.end is not None and len(args) > spec.end and result.kind == "addr" and len(result.addrs) == 1:
            stop = ev.eval(args[spec.end], m.start())
            if stop.kind == "addr" and len(stop.addrs) == 1 and stop.addrs[0] > result.addrs[0]:
                size = stop.addrs[0] - result.addrs[0]
        if name == "InjectHook" and len(args) > 2:
            kind = args[2].split("::")[-1].strip()
            spec_op = {"Jump": "inject_jump", "Call": "inject_call"}.get(kind, "inject_hook")
        else:
            spec_op = spec.op
        value = Evaluator.redact(args[spec.value])[:60] if spec.value is not None and len(args) > spec.value else ""
        target = ""
        if spec.target is not None and len(args) > spec.target:
            target = Evaluator.redact(args[spec.target]).lstrip("&")[:60]
        line = src.line_of(m.start())
        sites.append(Site(
            op=spec_op, file=rel, line=line, scope=scope_name(here), group=heading_for(src, frames, line, m.start()),
            cond=cond_text(here), expr=Evaluator.redact(addr_expr)[:80], value=value, target=target, size=size,
            result=result, version=version_for(cfg, here, result), pos=m.start()))

    def inside_op(pos: int) -> bool:
        return any(a <= pos <= b for a, b in consumed)

    for m in BIND_RX.finditer(src.masked):
        if inside_op(m.start()):
            continue
        p = m.end()
        while p < len(src.masked) and src.masked[p].isspace():
            p += 1
        if p < len(src.masked) and src.masked[p] == "<":
            close = _template_close(src.masked, p)
            if close < 0:
                continue
            p = close + 1
            while p < len(src.masked) and src.masked[p].isspace():
                p += 1
            if m.group(1).startswith("External"):
                nm = re.match(r"\s*[A-Za-z_][\w:]*\s*", src.masked[p:])
                if nm:
                    p += nm.end()
        elif m.group(1).startswith("External"):
            nm = re.match(r"\s*[A-Za-z_][\w:]*\s*", src.masked[p:])
            if nm:
                p += nm.end()
        if p >= len(src.masked) or src.masked[p] != "(":
            continue
        end = Evaluator._close_paren(src.masked, p)
        if end < 0:
            continue
        args = split_top(src.code[p + 1:end]) if src.code[p + 1:end].strip() else []
        if not args:
            continue
        here = frames_at(frames, m.start())
        if m.group(1) == "AddressByVersion":
            result = ev.eval(src.code[m.start():end + 1], m.start())
            expr = args[0]
        else:
            if ev._string_arg(args[:1]) is None:
                expr = args[0]
                result = ev.eval(expr, m.start())
            else:
                result = ev._call("hook::get_pattern", "", args, m.start())
                expr = Evaluator.redact(", ".join(args))
        if result.kind == "unknown" and m.group(1).startswith("External") and "AddressByVersion" in src.code[p:end]:
            continue
        line = src.line_of(m.start())
        consumed.append((m.start(), end))
        sites.append(Site(
            op="bind", file=rel, line=line, scope=scope_name(here), group=heading_for(src, frames, line, m.start()),
            cond=cond_text(here), expr=Evaluator.redact(expr)[:80], value="", target="", size=None, result=result,
            version=version_for(cfg, here, result), pos=m.start()))

    for m in DEREF_RX.finditer(src.masked):
        if inside_op(m.start()):
            continue
        addr = int(m.group(1), 16)
        if addr < 0x400000:
            continue
        here = frames_at(frames, m.start())
        line = src.line_of(m.start())
        result = Value("addr", [addr], ["literal"])
        sites.append(Site(
            op="bind", file=rel, line=line, scope=scope_name(here), group=heading_for(src, frames, line, m.start()),
            cond=cond_text(here), expr=Evaluator.redact(src.code[m.start():m.end()])[:80], value="", target="",
            size=None, result=result, version=version_for(cfg, here, result), pos=m.start()))
    sites.sort(key=lambda s: s.pos)
    return sites


def expand_files(src_root: Path, patterns: Sequence[str]) -> List[str]:
    out = []
    for pat in patterns:
        hits = sorted(glob.glob(str(src_root / pat)))
        for h in hits:
            rel = str(Path(h).relative_to(src_root))
            if rel not in out:
                out.append(rel)
    return out


@dataclass
class Row:
    id: str
    op: str
    addr: Optional[int]
    span: Optional[int]
    pin: str
    version: str
    group: str
    scope: str
    cond: str
    target: str
    value: str
    expr: str
    file: str
    line: int
    note: str


def group_name(s: "Site") -> str:
    if s.op == "bind":
        return f"bindings: {Path(s.file).name}"
    return s.group or s.scope or f"{Path(s.file).name} (file scope)"


def make_rows(prefix: str, sites: List[Site]) -> Tuple[List[Row], Dict[str, int]]:
    rows: List[Row] = []
    dropped: Dict[str, int] = {}
    seen: Dict[str, int] = {}
    for s in sites:
        if s.version not in KEPT_VERSIONS:
            dropped[s.version] = dropped.get(s.version, 0) + 1
            continue
        key = "|".join([s.file, s.scope, s.op, s.expr, s.target, s.value])
        nth = seen.get(key, 0)
        seen[key] = nth + 1
        digest = hashlib.sha1(f"{key}|{nth}".encode()).hexdigest()[:8]
        base_id = f"{prefix}-{digest}"
        r = s.result
        group = group_name(s)
        if r.kind == "addr" and r.addrs:
            pin = "+".join(r.pins) if r.pins else "literal"
            note = "; ".join(dict.fromkeys(r.notes))
            multi = len(r.addrs) > 1
            for k, a in enumerate(r.addrs):
                rows.append(Row(f"{base_id}.{k}" if multi else base_id, s.op, a, s.size, pin, s.version, group,
                                s.scope, s.cond, s.target, s.value, s.expr, s.file, s.line, note))
        else:
            reason = r.reason if r.kind == "unknown" else "address did not resolve"
            rows.append(Row(base_id, s.op, None, s.size, "unresolved", s.version, group, s.scope, s.cond,
                            s.target, s.value, s.expr, s.file, s.line, reason))
    return rows, dropped


def yq(v) -> str:
    if v is None:
        return "~"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    s = str(v).replace("\\", "\\\\").replace('"', '\\"')
    return f'"{s}"'


ROW_FIELDS = ("id", "op", "addr", "span", "pin", "version", "group", "scope", "cond", "target", "value", "expr", "file",
              "line", "note")


def write_inventory(path: Path, mod: str, cfg: dict, rows: List[Row], dropped: Dict[str, int], files: List[str]) -> None:
    out = [f"mod: {mod}", f"upstream: {yq(cfg['repo'])}", f"commit: {yq(cfg['commit'])}", f"files: {len(files)}",
           f"sites: {len(rows)}", f"unresolved: {sum(1 for r in rows if r.addr is None)}", "dropped:"]
    for k in sorted(dropped):
        out.append(f"  {yq(k)}: {dropped[k]}")
    if not dropped:
        out[-1] = "dropped: {}"
    out.append("rows:")
    for r in rows:
        first = True
        for f in ROW_FIELDS:
            v = getattr(r, f)
            if f in ("cond", "target", "value", "note", "scope") and v in ("", None):
                continue
            if f == "addr" and v is not None:
                text = f"0x{v:08x}"
            else:
                text = yq(v)
            out.append(("  - " if first else "    ") + f"{f}: {text}")
            first = False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(out) + "\n")


def _scalar(text: str):
    t = text.strip()
    if t == "~" or t == "":
        return None
    if t == "{}":
        return {}
    if t == "[]":
        return []
    if t in ("true", "false"):
        return t == "true"
    if t.startswith('"'):
        if not t.endswith('"') or len(t) < 2:
            raise ValueError(f"unterminated string: {t}")
        body = t[1:-1]
        out = []
        i = 0
        while i < len(body):
            if body[i] == "\\" and i + 1 < len(body):
                out.append(body[i + 1])
                i += 2
            else:
                out.append(body[i])
                i += 1
        return "".join(out)
    if re.fullmatch(r"-?\d+", t):
        return int(t)
    if re.fullmatch(r"0x[0-9a-fA-F]+", t):
        return int(t, 16)
    return t


def read_yaml(text: str) -> dict:
    root: dict = {}
    key_list: Optional[str] = None
    key_map: Optional[str] = None
    item: Optional[dict] = None
    sub_list: Optional[str] = None
    for lineno, raw in enumerate(text.splitlines(), 1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        indent = len(raw) - len(raw.lstrip(" "))
        body = raw.strip()
        try:
            if indent == 0:
                k, _, v = body.partition(":")
                key_list = key_map = None
                item = None
                sub_list = None
                if v.strip() == "":
                    root[k] = None
                    key_list = k
                    key_map = k
                else:
                    root[k] = _scalar(v)
                continue
            if indent == 2 and body.startswith("- "):
                if root.get(key_list) is None:
                    root[key_list] = []
                key_map = None
                item = {}
                root[key_list].append(item)
                sub_list = None
                body = body[2:]
                k, _, v = body.partition(":")
                item[k.strip()] = _scalar(v)
                continue
            if indent == 2 and key_map is not None and item is None:
                if root.get(key_map) is None:
                    root[key_map] = {}
                k, _, v = body.partition(":")
                root[key_map][_scalar(k)] = _scalar(v)
                continue
            if indent == 4 and item is not None and not body.startswith("- "):
                k, _, v = body.partition(":")
                if v.strip() == "":
                    item[k.strip()] = []
                    sub_list = k.strip()
                else:
                    item[k.strip()] = _scalar(v)
                    sub_list = None
                continue
            if indent == 6 and item is not None and sub_list and body.startswith("- "):
                item[sub_list].append(_scalar(body[2:]))
                continue
        except ValueError as e:
            raise ValueError(f"line {lineno}: {e}")
        raise ValueError(f"line {lineno}: unexpected layout: {raw!r}")
    return root


HINTS = [
    (re.compile(r"\bFLAC\b", re.I), "drop", "FLAC is dropped (ROADMAP §0)"),
    (re.compile(r"\bINI\b|\.ini\b|settings? file", re.I), "drop", "INI parsing is replaced by the unified settings file"),
    (re.compile(r"frame ?limiter|framerate|frame rate|\bFPS\b|frame ?delay|vsync", re.I), "replace", "frame limiter and timing are replaced natively (M9)"),
    (re.compile(r"Direct3D|DirectX|DirectInput|\bDInput|DirectSound|DSound|\bEAX\b|\bD3D9?\b", re.I), "replace", "D3D9/DInput/DSound paths live in the native layers"),
    (re.compile(r"resolution (?:selection|dialog|list)|native resolution|display mode|video mode|refresh rate|VRAM|"
                r"dual monitor|alt ?\+? ?-?tab|borderless|WM_\w+|mouse re-?centering", re.I), "replace", "window, monitor and mode handling are native"),
    (re.compile(r"\b(?:SAMP|SA-MP|LS-?RP|ImVehFt|SARender|outfit|modloader|CLEO|ASI)\b", re.I), "drop", "compatibility with other Windows mods"),
]


def hint_for(group: str, rows: List[Row]) -> Tuple[str, str]:
    text = " ".join([group] + [r.scope for r in rows] + [r.cond for r in rows])
    for rx, decision, reason in HINTS:
        if rx.search(text):
            return decision, reason
    return "", ""


def changelog_lines(path: Path) -> List[Tuple[int, set]]:
    if not path.is_file():
        return []
    out = []
    for n, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if line.lstrip().startswith(("*", "-")):
            words = set(w.lower() for w in re.findall(r"[A-Za-z]{4,}", line))
            if words:
                out.append((n, words))
    return out


STOPWORDS = {"with", "that", "this", "from", "when", "were", "would", "into", "have", "being", "they", "their", "should",
             "fixed", "fixes", "made", "make", "also", "game", "only", "more", "than", "them", "some", "after", "before",
             "properly", "proper", "anymore", "longer", "again", "same", "will", "does", "other"}


def changelog_for(group: str, lines: List[Tuple[int, set]], name: str) -> str:
    words = set(w.lower() for w in re.findall(r"[A-Za-z]{4,}", group)) - STOPWORDS
    if len(words) < 2 or not lines:
        return ""
    best = (0.0, 0)
    for n, lw in lines:
        hit = len(words & lw)
        score = hit / len(words)
        if hit >= 2 and score > best[0]:
            best = (score, n)
    return f"{name}:{best[1]}" if best[0] >= 0.6 else ""


def build_triage(mod: str, cfg: dict, rows: List[Row], existing: Optional[dict], changelog: Optional[Path]) -> Tuple[str, List[str]]:
    groups: Dict[str, List[Row]] = {}
    for r in rows:
        groups.setdefault(r.group, []).append(r)
    old = {}
    if existing:
        for g in existing.get("groups") or []:
            old[g.get("group")] = g
    lines = changelog_lines(changelog) if changelog else []
    out = [f"mod: {mod}", f"commit: {yq(cfg['commit'])}", "groups:"]
    for name, members in groups.items():
        prev = old.get(name, {})
        decision = prev.get("decision") or "pending"
        reason = prev.get("reason") or ""
        hint_decision, hint_reason = hint_for(name, members)
        clog = prev.get("changelog") or changelog_for(name, lines, cfg.get("changelog") or "")
        out.append(f"  - group: {yq(name)}")
        out.append(f"    decision: {decision}")
        out.append(f"    reason: {yq(reason)}")
        if hint_decision and decision == "pending":
            out.append(f"    hint: {yq(hint_decision + ': ' + hint_reason)}")
        out.append(f"    changelog: {yq(clog)}")
        out.append("    sites:")
        for r in members:
            out.append(f"      - {r.id}")
    gone = [g for g in old if g not in groups and (old[g].get("decision") or "pending") != "pending"]
    return "\n".join(out) + "\n", gone


def rows_from_inventory(data: dict) -> List[Row]:
    rows = []
    for d in data.get("rows") or []:
        rows.append(Row(**{f: d.get(f, "" if f not in ("addr", "span", "line") else None) for f in ROW_FIELDS}))
    return rows


def check_mod(mod: str, inv: dict, tri: Optional[dict]) -> Tuple[List[str], Dict[str, int]]:
    errors: List[str] = []
    stats: Dict[str, int] = {d: 0 for d in DECISIONS}
    rows = inv.get("rows") or []
    ids = {}
    for r in rows:
        for f in ("id", "op", "pin", "version", "group", "file", "line"):
            if r.get(f) in (None, ""):
                errors.append(f"{mod}: row {r.get('id')} has no {f}")
        if r.get("id") in ids:
            errors.append(f"{mod}: duplicate site id {r.get('id')}")
        ids[r.get("id")] = r
        if r.get("version") not in KEPT_VERSIONS:
            errors.append(f"{mod}: row {r.get('id')} has version {r.get('version')}, which should have been dropped")
        if r.get("addr") is None and r.get("pin") != "unresolved":
            errors.append(f"{mod}: row {r.get('id')} has no addr but pin {r.get('pin')}")
    if tri is None:
        errors.append(f"{mod}: triage file is missing")
        return errors, stats
    placed: Dict[str, str] = {}
    for g in tri.get("groups") or []:
        name = g.get("group")
        decision = g.get("decision")
        if decision not in DECISIONS:
            errors.append(f"{mod}: group {name!r} has decision {decision!r}; expected one of {', '.join(DECISIONS)}")
        elif decision != "pending" and not g.get("reason"):
            errors.append(f"{mod}: group {name!r} is {decision} without a reason")
        for sid in g.get("sites") or []:
            if sid not in ids:
                errors.append(f"{mod}: group {name!r} lists unknown site {sid}")
            elif sid in placed:
                errors.append(f"{mod}: site {sid} is in groups {placed[sid]!r} and {name!r}")
            else:
                placed[sid] = name
                if decision in stats:
                    stats[decision] += 1
    for sid in ids:
        if sid not in placed:
            errors.append(f"{mod}: site {sid} is not in any triage group")
    return errors, stats


def overlaps(all_rows: Dict[str, List[dict]], decisions: Dict[str, Dict[str, str]]) -> List[str]:
    spans = []
    for mod, rows in all_rows.items():
        for r in rows:
            if r.get("addr") is None or r.get("op") in ("bind", "read", "read_call", "verify", "read_offset", "read_displacement"):
                continue
            if decisions.get(mod, {}).get(r["id"]) == "drop":
                continue
            span = r.get("span") or 1
            spans.append((r["addr"], r["addr"] + span, mod, r["id"]))
    spans.sort()
    out = []
    for i, (a0, a1, mod, sid) in enumerate(spans):
        for b0, b1, mod2, sid2 in spans[i + 1:]:
            if b0 >= a1:
                break
            if mod != mod2:
                out.append(f"0x{a0:08x}+{a1 - a0} {mod} {sid} overlaps 0x{b0:08x}+{b1 - b0} {mod2} {sid2}")
    return out


def cmd_fetch(args) -> int:
    cfg = load_config()
    for mod in args.mods or list(cfg):
        c = cfg[mod]
        dest = CACHE / mod
        if not (dest / ".git").exists():
            dest.mkdir(parents=True, exist_ok=True)
            subprocess.run(["git", "init", "-q", str(dest)], check=True)
            subprocess.run(["git", "-C", str(dest), "remote", "add", "origin", c["repo"] + ".git"], check=True)
        head = subprocess.run(["git", "-C", str(dest), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        if head != c["commit"]:
            subprocess.run(["git", "-C", str(dest), "fetch", "-q", "--depth", "1", "origin", c["commit"]], check=True)
            subprocess.run(["git", "-C", str(dest), "checkout", "-q", "FETCH_HEAD"], check=True)
        subprocess.run(["git", "-C", str(dest), "submodule", "update", "--init", "--depth", "1", "-q"], check=True)
        print(f"{mod}: {c['commit']} in {dest}")
    return 0


def scan_mod(mod: str, cfg: dict, src_root: Path, image: Optional[Image]) -> Tuple[List[Row], Dict[str, int], List[str]]:
    head = subprocess.run(["git", "-C", str(src_root), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if head and head != cfg["commit"]:
        raise SystemExit(f"{mod}: {src_root} is at {head}, expected pinned {cfg['commit']} (run fetch)")
    files = expand_files(src_root, cfg["files"])
    if not files:
        raise SystemExit(f"{mod}: no source files under {src_root}")
    sites: List[Site] = []
    for rel in files:
        text = (src_root / rel).read_text(errors="replace")
        sites.extend(scan_text(cfg, rel, text, image))
    rows, dropped = make_rows(cfg["prefix"], sites)
    return rows, dropped, files


def cmd_scan(args) -> int:
    cfg = load_config()
    image = Image.load(Path(args.exe)) if args.exe else None
    for mod in args.mods or list(cfg):
        c = cfg[mod]
        src_root = Path(args.src) / mod if args.src else CACHE / mod
        rows, dropped, files = scan_mod(mod, c, src_root, image)
        out_dir = Path(args.out) if args.out else MODS_DIR / mod
        pins_path = out_dir / "pins.yaml"
        if pins_path.exists():
            rows = apply_pins(mod, rows, read_yaml(pins_path.read_text()))
        if image:
            for r in rows:
                if r.addr is not None and not image.contains(r.addr) and r.op != "bind":
                    r.note = "; ".join(x for x in (r.note, "address is outside the image") if x)
        inv_path = out_dir / "inventory.yaml"
        write_inventory(inv_path, mod, c, rows, dropped, files)
        tri_path = out_dir / "triage.yaml"
        existing = read_yaml(tri_path.read_text()) if tri_path.exists() else None
        changelog = src_root / c["changelog"] if c.get("changelog") else None
        text, gone = build_triage(mod, c, rows, existing, changelog)
        tri_path.write_text(text)
        unresolved = sum(1 for r in rows if r.addr is None)
        drop_text = ", ".join(f"{k} {v}" for k, v in sorted(dropped.items())) or "none"
        print(f"{mod}: {len(rows)} sites from {len(files)} files, {unresolved} unresolved, dropped {drop_text}")
        for g in gone:
            print(f"{mod}: triaged group {g!r} no longer has sites; its decision was discarded", file=sys.stderr)
    return 0


def apply_pins(mod: str, rows: List[Row], pins: dict) -> List[Row]:
    by_id = {r.id: r for r in rows}
    expanded: Dict[str, List[Row]] = {}
    for p in pins.get("pins") or []:
        r = by_id.get(p.get("id"))
        if r is None:
            print(f"{mod}: pin for unknown site {p.get('id')}", file=sys.stderr)
            continue
        if not p.get("reason"):
            print(f"{mod}: pin for {r.id} needs a reason", file=sys.stderr)
            continue
        if p.get("addr") is None and p.get("addrs") is None:
            if not p.get("group"):
                print(f"{mod}: pin for {r.id} needs an addr, addrs or group", file=sys.stderr)
                continue
            r.group = p["group"]
            continue
        addrs = p.get("addrs") if isinstance(p.get("addrs"), list) else [p.get("addr")]
        if not addrs or not all(isinstance(a, int) for a in addrs):
            print(f"{mod}: pin for {r.id} has an addr that is not a number", file=sys.stderr)
            continue
        copies = []
        for k, a in enumerate(addrs):
            c = replace(r, id=f"{r.id}.{k}") if len(addrs) > 1 else r
            c.addr = a
            c.pin = "manual"
            if p.get("span"):
                c.span = p["span"]
            c.note = p["reason"]
            if p.get("group"):
                c.group = p["group"]
            copies.append(c)
        expanded[r.id] = copies
    return [c for r in rows for c in expanded.get(r.id, [r])]


def read_manifest_entries(directory: Path) -> List[dict]:
    raw_entries: List[dict] = []
    for path in sorted(list(directory.glob("*.yaml")) + list(directory.glob("*.yml"))):
        current: Optional[dict] = None
        last = None
        for lineno, raw in enumerate(path.read_text().splitlines(), 1):
            body = raw.split(" #")[0].rstrip() if not raw.lstrip().startswith("#") else ""
            if not body.strip():
                continue
            if body.startswith("- "):
                current = {"file": path.name, "line": lineno}
                raw_entries.append(current)
                body = body[2:]
                last = None
            elif current is None:
                continue
            elif last and current[last].endswith(","):
                current[last] += " " + body.strip()
                continue
            k, _, v = body.strip().partition(":")
            last = k.strip()
            current[last] = v.strip()
    entries: List[dict] = []
    for e in raw_entries:
        fields = {k: (v if k in ("file", "line") else _scalar(v)) for k, v in e.items()}
        addrs = [a.strip() for a in e.get("addr", "").split(",") if a.strip()]
        if len(addrs) <= 1:
            entries.append(fields)
            continue
        for a in addrs:
            entries.append(dict(fields, addr=_scalar(a)))
    return entries


def manifest_span(e: dict) -> int:
    kind = e.get("kind")
    if kind in ("byte_patch", "data_poke"):
        return max(1, len(re.sub(r"\s", "", str(e.get("bytes") or ""))) // 2)
    if kind == "nop":
        return int(e.get("len") or 1)
    if kind in ("call_redirect", "mid_hook"):
        return int(e.get("len") or 5)
    if kind == "operand_slot":
        return 6
    if kind == "data_slot":
        return 4
    return 1


def manifest_errors(entries: List[dict], all_rows: Dict[str, List[dict]], decisions: Dict[str, Dict[str, str]]) -> List[str]:
    errors = []
    groups: Dict[str, Dict[str, str]] = {}
    for e in entries:
        source = e.get("source")
        if source in (None, "native"):
            continue
        where = f"{e['file']}:{e['line']}"
        addr = e.get("addr")
        if source not in all_rows:
            errors.append(f"{where}: {source} entry at {addr!r} but there is no {source} inventory")
            continue
        if not isinstance(addr, int):
            errors.append(f"{where}: addr {addr!r} is not a number")
            continue
        if e.get("site"):
            hits = [r for r in all_rows[source] if r["id"] == e["site"]]
            if not hits:
                errors.append(f"{where}: site {e['site']} is not in the {source} inventory")
                continue
        else:
            end = addr + manifest_span(e)
            hits = [r for r in all_rows[source] if r.get("addr") is not None and r["addr"] < end and addr < r["addr"] + max(1, r.get("span") or 1)]
            if not hits:
                errors.append(f"{where}: {source} entry at 0x{addr:08x} matches no {source} inventory site")
                continue
        kept = [r for r in hits if decisions.get(source, {}).get(r["id"]) == "keep"]
        if not kept:
            states = ", ".join(f"{r['id']} {decisions.get(source, {}).get(r['id'], 'untriaged')}" for r in hits)
            errors.append(f"{where}: {source} entry at 0x{addr:08x} ports a site whose group is not kept ({states})")
    return errors


def cmd_check(args) -> int:
    cfg = load_config()
    root = Path(args.root) if args.root else MODS_DIR
    errors: List[str] = []
    all_rows: Dict[str, List[dict]] = {}
    decisions: Dict[str, Dict[str, str]] = {}
    found = 0
    for mod in args.mods or list(cfg):
        inv_path = root / mod / "inventory.yaml"
        if not inv_path.exists():
            if args.require:
                errors.append(f"{mod}: {inv_path} is missing")
            continue
        found += 1
        try:
            inv = read_yaml(inv_path.read_text())
        except ValueError as e:
            errors.append(f"{inv_path}: {e}")
            continue
        if inv.get("commit") != cfg[mod]["commit"]:
            errors.append(f"{mod}: inventory is for {inv.get('commit')}, pinned commit is {cfg[mod]['commit']}")
        tri_path = root / mod / "triage.yaml"
        tri = None
        if tri_path.exists():
            try:
                tri = read_yaml(tri_path.read_text())
            except ValueError as e:
                errors.append(f"{tri_path}: {e}")
        errs, stats = check_mod(mod, inv, tri)
        errors.extend(errs)
        if args.strict and stats.get("pending"):
            errors.append(f"{mod}: {stats['pending']} sites are still pending triage")
        all_rows[mod] = inv.get("rows") or []
        decisions[mod] = {}
        for g in (tri or {}).get("groups") or []:
            for sid in g.get("sites") or []:
                decisions[mod][sid] = g.get("decision")
        unresolved = sum(1 for r in all_rows[mod] if r.get("addr") is None)
        print(f"{mod}: {len(all_rows[mod])} sites, {unresolved} unresolved, " + ", ".join(f"{k} {v}" for k, v in stats.items()))
    for line in overlaps(all_rows, decisions):
        print(f"cross-mod: {line}")
    if args.manifest:
        entries = read_manifest_entries(Path(args.manifest))
        errors.extend(manifest_errors(entries, all_rows, decisions))
        ported = sum(1 for e in entries if e.get("source") not in (None, "native"))
        print(f"manifest: {len(entries)} entries, {ported} from mods")
    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    if not found and not args.require:
        print("no inventories found")
    return 1 if errors else 0


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(description="Inventory upstream mod patch sites as 1.0 US addresses")
    sub = p.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch", help="clone the pinned upstream commits into .cache/mods")
    f.add_argument("mods", nargs="*")
    s = sub.add_parser("scan", help="write mods/<mod>/inventory.yaml and merge mods/<mod>/triage.yaml")
    s.add_argument("mods", nargs="*")
    s.add_argument("--exe", help="gta_sa.exe 1.0 US, used to resolve pattern scans and branch targets")
    s.add_argument("--src", help="directory holding <mod>/ checkouts (default .cache/mods)")
    s.add_argument("--out", help="output directory (default mods/<mod>)")
    c = sub.add_parser("check", help="validate inventories and triage records")
    c.add_argument("mods", nargs="*")
    c.add_argument("--root", help="directory holding <mod>/inventory.yaml (default mods)")
    c.add_argument("--require", action="store_true", help="fail when an inventory is missing")
    c.add_argument("--strict", action="store_true", help="fail while any site is pending triage")
    c.add_argument("--manifest", help="manifest directory; every mod entry must port a site whose group is kept")
    args = p.parse_args(argv)
    return {"fetch": cmd_fetch, "scan": cmd_scan, "check": cmd_check}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
