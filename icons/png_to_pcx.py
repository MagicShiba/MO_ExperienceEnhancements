#!/usr/bin/env python3
"""Convert PNG files in this folder to engine PCX.

Output rules match pcx_tool.py:
  - text icons (TEXT_ICONS in pcx_tool.py) -> 1 plane 8bpp grayscale coverage
  - other icons -> 3 plane RGB, alpha < 128 becomes magenta (255, 0, 255)

Requires Pillow: pip install pillow

Usage:
  python icons/png_to_pcx.py                 # convert *.png next to this script
  python icons/png_to_pcx.py foo.png bar.png # convert selected files
  python icons/png_to_pcx.py --text icon_x.png --color border.png
  python icons/png_to_pcx.py --out build/icons --recursive

Run pcx_tool.py afterwards only for existing PCX files; PNG output is already
normalized and can be embedded by src/GUI/icons.rc.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.dont_write_bytecode = True

try:
    from PIL import Image
except ImportError:
    print("[png] error: Pillow is required (pip install pillow)", file=sys.stderr)
    raise SystemExit(1)

from pcx_tool import (
    MAGENTA,
    PcxError,
    TEXT_ICONS,
    write_gray,
    write_rgb,
)


def _load_rgba(path: Path) -> tuple[int, int, list[list[tuple[int, int, int, int]]]]:
    with Image.open(path) as im:
        im.load()
        rgba = im.convert("RGBA")
        w, h = rgba.size
        if w < 1 or h < 1 or w > 4096 or h > 4096:
            raise PcxError(f"bad PNG size {w}x{h}")
        raw = rgba.tobytes()
    rows: list[list[tuple[int, int, int, int]]] = []
    stride = w * 4
    for y in range(h):
        base = y * stride
        rows.append(
            [
                (raw[base + x * 4], raw[base + x * 4 + 1], raw[base + x * 4 + 2], raw[base + x * 4 + 3])
                for x in range(w)
            ]
        )
    return w, h, rows


def _is_text(name: str, text: set[str], color: set[str]) -> bool:
    stem = name.lower().rsplit(".", 1)[0]
    keys = {name.lower(), stem}
    if keys & color:
        return False
    if keys & text:
        return True
    return name.lower() in TEXT_ICONS


def convert(path: Path, out_dir: Path | None, text: set[str], color: set[str]) -> bool:
    w, h, rgba = _load_rgba(path)
    is_text = _is_text(path.name, text, color)
    dst = (out_dir or path.parent) / (path.stem + ".pcx")

    if is_text:
        has_alpha = any(a < 255 for row in rgba for _, _, _, a in row)
        coverage = [
            [a if has_alpha else max(r, g, b) for r, g, b, a in row]
            for row in rgba
        ]
        write_gray(dst, w, h, coverage)
        source = "alpha" if has_alpha else "max RGB"
        print(f"[png] {path.name} -> {dst.name}: 1-plane 8bpp gray ({source} coverage)")
        return True

    pixels: list[list[tuple[int, int, int]]] = []
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
    write_rgb(dst, w, h, pixels)
    if pixels[0][0] != MAGENTA:
        print(f"[png] warning {path.name}: top-left is not transparent; key will be {pixels[0][0]}")
    print(f"[png] {path.name} -> {dst.name}: 3-plane RGB ({transparent} transparent px)")
    return True


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Convert PNG icons to engine PCX.")
    ap.add_argument("files", nargs="*", help="PNG files or directories (default: script folder)")
    ap.add_argument("--out", help="output directory (default: next to each PNG)")
    ap.add_argument("--recursive", action="store_true", help="search directories recursively")
    ap.add_argument("--text", action="append", default=[], metavar="NAME",
                    help="force 8bpp text coverage output for this PNG name")
    ap.add_argument("--color", action="append", default=[], metavar="NAME",
                    help="force 3-plane RGB output for this PNG name")
    args = ap.parse_args(argv[1:])

    text = {n.lower() for n in args.text}
    color = {n.lower() for n in args.color}
    both = text & color
    if both:
        print(f"[png] error: name forced as both text and color: {', '.join(sorted(both))}")
        return 1

    targets = args.files or [str(Path(__file__).resolve().parent)]
    files: list[Path] = []
    for item in targets:
        p = Path(item)
        if p.is_dir():
            it = p.rglob("*.png") if args.recursive else p.glob("*.png")
            files.extend(sorted(it))
        elif p.is_file():
            files.append(p)
        else:
            print(f"[png] error: not found: {p}")
            return 1

    if not files:
        print("[png] no .png files")
        return 0

    out_dir = Path(args.out) if args.out else None
    if out_dir:
        out_dir.mkdir(parents=True, exist_ok=True)

    failed = 0
    for path in files:
        try:
            convert(path, out_dir, text, color)
        except Exception as exc:
            print(f"[png] error {path.name}: {exc}")
            failed += 1

    print(f"[png] converted {len(files) - failed}, failed {failed}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
