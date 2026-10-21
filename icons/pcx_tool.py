#!/usr/bin/env python3
"""Normalize PCX files in this folder for the engine.

1 plane = 8bpp index (text coverage); 3 plane = RGB (magenta transparent).
4-plane RGBA is converted in place: color -> 3-plane RGB, text -> 8bpp gray.
Run manually when icons change: `python icons/pcx_tool.py`, then rebuild
(build-fast.bat / build-full.bat) so icons.rc embeds the normalized files.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

MAGENTA = (255, 0, 255)
TEXT_ICONS = {
    "icon_power_up.pcx",
    "icon_power_down.pcx",
    "icon_time.pcx",
    "icon_cost.pcx",
}


class PcxError(Exception):
    pass


def _u16(data: bytes, off: int) -> int:
    return int.from_bytes(data[off : off + 2], "little", signed=False)


def _decode_rle(data: bytes, off: int, count: int) -> tuple[list[int], int]:
    out: list[int] = []
    n = len(data)
    while len(out) < count:
        if off >= n:
            raise PcxError("truncated RLE data")
        b = data[off]
        off += 1
        if b >= 0xC0:
            run = b & 0x3F
            if run == 0 or off >= n:
                raise PcxError("bad RLE run")
            v = data[off]
            off += 1
            out.extend([v] * run)
        else:
            out.append(b)
    return out[:count], off


def _encode_rle(values: list[int]) -> bytes:
    out = bytearray()
    i = 0
    n = len(values)
    while i < n:
        v = values[i]
        run = 1
        while i + run < n and values[i + run] == v and run < 63:
            run += 1
        if run > 1 or v >= 0xC0:
            out.append(0xC0 | run)
            out.append(v)
        else:
            out.append(v)
        i += run
    return bytes(out)


def read_pcx(path: Path) -> dict:
    data = path.read_bytes()
    if len(data) < 128:
        raise PcxError("file shorter than PCX header")
    if data[0] != 0x0A:
        raise PcxError("not a PCX file (manufacturer != 0x0A)")
    encoding = data[2]
    bpp = data[3]
    xmin, ymin = _u16(data, 4), _u16(data, 6)
    xmax, ymax = _u16(data, 8), _u16(data, 10)
    w = xmax - xmin + 1
    h = ymax - ymin + 1
    if w < 1 or h < 1 or w > 4096 or h > 4096:
        raise PcxError(f"bad PCX size {w}x{h}")
    nplanes = data[65]
    bpl = _u16(data, 66)
    if bpl < w:
        raise PcxError(f"bytes-per-line {bpl} < width {w}")
    if bpp != 8:
        raise PcxError(f"unsupported {nplanes}-plane {bpp}bpp PCX")
    if nplanes not in (1, 3, 4):
        raise PcxError(f"unsupported {nplanes}-plane PCX")

    off = 128
    planes: list[list[list[int]]] = []
    for _y in range(h):
        row: list[list[int]] = []
        for _pl in range(nplanes):
            if encoding == 0:
                if off + bpl > len(data):
                    raise PcxError("truncated raw scanline")
                raw = list(data[off : off + bpl])
                off += bpl
            elif encoding == 1:
                raw, off = _decode_rle(data, off, bpl)
            else:
                raise PcxError(f"unsupported PCX encoding {encoding}")
            row.append(raw)
        planes.append(row)

    palette = None
    if nplanes == 1 and len(data) >= 769 and data[-769] == 0x0C:
        tail = data[-768:]
        palette = [tuple(tail[i * 3 : i * 3 + 3]) for i in range(256)]

    return {
        "w": w,
        "h": h,
        "nplanes": nplanes,
        "bpp": bpp,
        "planes": planes,
        "palette": palette,
    }


def _header(w: int, h: int, nplanes: int, bpl: int) -> bytearray:
    hdr = bytearray(128)
    hdr[0] = 0x0A
    hdr[1] = 5
    hdr[2] = 1
    hdr[3] = 8
    hdr[4:6] = (0).to_bytes(2, "little")
    hdr[6:8] = (0).to_bytes(2, "little")
    hdr[8:10] = (w - 1).to_bytes(2, "little")
    hdr[10:12] = (h - 1).to_bytes(2, "little")
    hdr[12:14] = (96).to_bytes(2, "little")
    hdr[14:16] = (96).to_bytes(2, "little")
    hdr[65] = nplanes
    hdr[66:68] = bpl.to_bytes(2, "little")
    hdr[68:70] = (1).to_bytes(2, "little")
    return hdr


def _write_atomic(path: Path, payload: bytes) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(payload)
    os.replace(tmp, path)


def write_rgb(path: Path, w: int, h: int, pixels: list[list[tuple[int, int, int]]]) -> None:
    bpl = (w + 1) & ~1
    out = _header(w, h, 3, bpl)
    for y in range(h):
        for pl in range(3):
            line = [pixels[y][x][pl] for x in range(w)]
            line.extend([0] * (bpl - w))
            out.extend(_encode_rle(line))
    _write_atomic(path, bytes(out))


def write_gray(path: Path, w: int, h: int, coverage: list[list[int]]) -> None:
    bpl = (w + 1) & ~1
    out = _header(w, h, 1, bpl)
    for y in range(h):
        line = [coverage[y][x] for x in range(w)]
        line.extend([0] * (bpl - w))
        out.extend(_encode_rle(line))
    out.append(0x0C)
    for i in range(256):
        out.extend((i, i, i))
    _write_atomic(path, bytes(out))


def _rgba_rows(img: dict) -> list[list[tuple[int, int, int, int]]]:
    rows = []
    planes = img["planes"]
    for y in range(img["h"]):
        row = []
        for x in range(img["w"]):
            r, g, b, a = (planes[y][pl][x] for pl in range(4))
            row.append((r, g, b, a))
        rows.append(row)
    return rows


def convert(path: Path) -> bool:
    img = read_pcx(path)
    w, h = img["w"], img["h"]
    nplanes = img["nplanes"]
    text = path.name.lower() in TEXT_ICONS

    if nplanes == 1:
        if text:
            return False
        if not img["palette"]:
            raise PcxError("indexed color PCX has no palette")
        pal = img["palette"]
        rows = []
        for y in range(h):
            row = []
            for x in range(w):
                idx = img["planes"][y][0][x]
                row.append(MAGENTA if idx == 0 else pal[idx])
            rows.append(row)
        write_rgb(path, w, h, rows)
        print(f"[pcx] converted {path.name}: 8bpp index -> 24-bit RGB (index 0 transparent)")
        return True

    if nplanes == 3:
        if text:
            rows = []
            for y in range(h):
                row = []
                for x in range(w):
                    r, g, b = (img["planes"][y][pl][x] for pl in range(3))
                    row.append(max(r, g, b))
                rows.append(row)
            write_gray(path, w, h, rows)
            print(f"[pcx] converted {path.name}: 24-bit RGB -> 8bpp gray (max RGB as coverage)")
            return True
        top = tuple(img["planes"][0][pl][0] for pl in range(3))
        if top != MAGENTA:
            print(f"[pcx] warning {path.name}: top-left is {top}, not magenta")
        return False

    if nplanes != 4:
        raise PcxError(f"unsupported {nplanes}-plane PCX")

    rgba = _rgba_rows(img)
    if text:
        coverage = []
        has_alpha = any(a < 255 for row in rgba for _, _, _, a in row)
        for row in rgba:
            cov = []
            for r, g, b, a in row:
                cov.append(a if has_alpha else max(r, g, b))
            coverage.append(cov)
        write_gray(path, w, h, coverage)
        if not has_alpha:
            print(f"[pcx] warning {path.name}: RGBA alpha is fully opaque; used RGB luma")
        print(f"[pcx] converted {path.name}: RGBA -> 8bpp gray")
        return True

    pixels = []
    transparent = 0
    for row in rgba:
        out = []
        for r, g, b, a in row:
            if a < 128:
                out.append(MAGENTA)
                transparent += 1
            else:
                out.append((r, g, b))
        pixels.append(out)
    write_rgb(path, w, h, pixels)
    if pixels[0][0] != MAGENTA:
        print(f"[pcx] warning {path.name}: top-left is not transparent; key will be {pixels[0][0]}")
    if transparent == 0:
        print(f"[pcx] warning {path.name}: no alpha < 128; image has no transparent pixels")
    print(f"[pcx] converted {path.name}: 4-plane RGBA -> 3-plane RGB ({transparent} transparent px)")
    return True


def main(argv: list[str]) -> int:
    targets = argv[1:] or [str(Path(__file__).resolve().parent)]

    files: list[Path] = []
    for item in targets:
        p = Path(item)
        if p.is_dir():
            files.extend(sorted(p.glob("*.pcx")))
        elif p.is_file():
            files.append(p)
        else:
            print(f"[pcx] error: not found: {p}")
            return 1

    if not files:
        print("[pcx] no .pcx files")
        return 0

    changed = 0
    failed = 0
    for path in files:
        try:
            if convert(path):
                changed += 1
        except PcxError as exc:
            print(f"[pcx] error {path.name}: {exc}")
            failed += 1
        except OSError as exc:
            print(f"[pcx] error {path.name}: {exc}")
            failed += 1

    print(f"[pcx] checked {len(files)}, converted {changed}, failed {failed}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
