#!/usr/bin/env python3
import argparse
import struct
import sys
import zlib
from pathlib import Path
from typing import List, Optional, Tuple

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


class Image:
    def __init__(self, width: int, height: int, rgb: bytearray):
        self.width = width
        self.height = height
        self.rgb = rgb

    def pixel(self, x: int, y: int) -> Tuple[int, int, int]:
        i = (y * self.width + x) * 3
        return self.rgb[i], self.rgb[i + 1], self.rgb[i + 2]


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path: Path) -> Image:
    data = path.read_bytes()
    if not data.startswith(PNG_MAGIC):
        raise ValueError(f"{path} is not a PNG")
    pos = 8
    width = height = 0
    depth = color = interlace = 0
    idat = bytearray()
    palette = b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if depth != 8 or interlace != 0 or color not in (0, 2, 3, 4, 6):
        raise ValueError(f"{path}: only 8-bit non-interlaced PNGs are supported")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    stride = width * channels
    raw = zlib.decompress(bytes(idat))
    out = bytearray(width * height * 3)
    prev = bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = prev[i]
            c = prev[i - channels] if i >= channels else 0
            if f == 1:
                line[i] = (line[i] + a) & 0xFF
            elif f == 2:
                line[i] = (line[i] + b) & 0xFF
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif f == 4:
                line[i] = (line[i] + _paeth(a, b, c)) & 0xFF
        prev = line
        for x in range(width):
            o = (y * width + x) * 3
            if color == 2 or color == 6:
                out[o:o + 3] = line[x * channels:x * channels + 3]
            elif color == 3:
                k = line[x] * 3
                out[o:o + 3] = palette[k:k + 3]
            else:
                g = line[x * channels]
                out[o:o + 3] = bytes((g, g, g))
    return Image(width, height, out)


def write_png(path: Path, img: Image) -> None:
    rows = bytearray()
    for y in range(img.height):
        rows.append(0)
        rows += img.rgb[y * img.width * 3:(y + 1) * img.width * 3]

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", img.width, img.height, 8, 2, 0, 0, 0)
    path.write_bytes(PNG_MAGIC + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(rows), 6)) +
                     chunk(b"IEND", b""))


class Result:
    def __init__(self, compared: int, differing: int, max_delta: int, bbox: Optional[Tuple[int, int, int, int]]):
        self.compared = compared
        self.differing = differing
        self.max_delta = max_delta
        self.bbox = bbox

    @property
    def fraction(self) -> float:
        return self.differing / self.compared if self.compared else 0.0


def parse_rects(specs: List[str]) -> List[Tuple[int, int, int, int]]:
    out = []
    for spec in specs:
        x, y, w, h = (int(v) for v in spec.split(","))
        out.append((x, y, w, h))
    return out


def crop(img: Image, rect: Tuple[int, int, int, int]) -> Image:
    x, y, w, h = rect
    if x < 0 or y < 0 or w <= 0 or h <= 0 or x + w > img.width or y + h > img.height:
        raise ValueError(f"region {x},{y},{w},{h} is outside the {img.width}x{img.height} image")
    out = bytearray(w * h * 3)
    for row in range(h):
        start = ((y + row) * img.width + x) * 3
        out[row * w * 3:(row + 1) * w * 3] = img.rgb[start:start + w * 3]
    return Image(w, h, out)


def compare(a: Image, b: Image, tolerance: int, ignore: List[Tuple[int, int, int, int]] = (),
            diff: Optional[Image] = None) -> Result:
    if (a.width, a.height) != (b.width, b.height):
        raise ValueError(f"sizes differ: {a.width}x{a.height} vs {b.width}x{b.height}")
    compared = differing = max_delta = 0
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    for y in range(a.height):
        for x in range(a.width):
            if any(rx <= x < rx + rw and ry <= y < ry + rh for rx, ry, rw, rh in ignore):
                continue
            i = (y * a.width + x) * 3
            d = max(abs(a.rgb[i] - b.rgb[i]), abs(a.rgb[i + 1] - b.rgb[i + 1]), abs(a.rgb[i + 2] - b.rgb[i + 2]))
            compared += 1
            max_delta = max(max_delta, d)
            if d > tolerance:
                differing += 1
                x0, y0, x1, y1 = min(x0, x), min(y0, y), max(x1, x), max(y1, y)
                if diff is not None:
                    diff.rgb[i:i + 3] = bytes((255, 0, 255))
            elif diff is not None:
                g = (a.rgb[i] + a.rgb[i + 1] + a.rgb[i + 2]) // 12
                diff.rgb[i:i + 3] = bytes((g, g, g))
    bbox = (x0, y0, x1 - x0 + 1, y1 - y0 + 1) if x1 >= 0 else None
    return Result(compared, differing, max_delta, bbox)


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(description="Compare a capture against a reference screenshot within a tolerance")
    p.add_argument("capture", type=Path)
    p.add_argument("reference", type=Path)
    p.add_argument("--tolerance", type=int, default=8, help="per-channel difference a pixel may have (0-255)")
    p.add_argument("--max-fraction", type=float, default=0.001, help="fraction of pixels allowed to exceed it")
    p.add_argument("--ignore", action="append", default=[], metavar="X,Y,W,H", help="rectangle to skip (clock, FPS)")
    p.add_argument("--diff", type=Path, help="write a PNG with differing pixels in magenta")
    p.add_argument("--region", metavar="X,Y,W,H", help="compare only this rectangle of the capture")
    p.add_argument("--reference-region", metavar="X,Y,W,H", help="rectangle of the reference (default: --region)")
    args = p.parse_args(argv)
    a = read_png(args.capture)
    b = read_png(args.reference)
    try:
        if args.region:
            a = crop(a, parse_rects([args.region])[0])
        if args.region or args.reference_region:
            b = crop(b, parse_rects([args.reference_region or args.region])[0])
    except ValueError as e:
        print(f"image-diff: {e}", file=sys.stderr)
        return 2
    diff = Image(a.width, a.height, bytearray(a.width * a.height * 3)) if args.diff else None
    try:
        r = compare(a, b, args.tolerance, parse_rects(args.ignore), diff)
    except ValueError as e:
        print(f"image-diff: {e}", file=sys.stderr)
        return 2
    if diff is not None:
        write_png(args.diff, diff)
    verdict = "match" if r.fraction <= args.max_fraction else "differ"
    box = f", in {r.bbox[2]}x{r.bbox[3]} at {r.bbox[0]},{r.bbox[1]}" if r.bbox else ""
    print(f"image-diff: {verdict}: {r.differing} of {r.compared} pixels ({r.fraction:.4%}) exceed {args.tolerance}, "
          f"max delta {r.max_delta}{box}")
    return 0 if verdict == "match" else 1


if __name__ == "__main__":
    sys.exit(main())
