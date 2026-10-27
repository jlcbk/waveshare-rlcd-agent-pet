#!/usr/bin/env python3
"""Regenerate src/doto_font.cpp with Inter glyphs (SIL Open Font License 1.1).

The screen font used to be the dot-matrix face "Doto"; this tool rasterizes
Inter (https://github.com/rsms/inter, OFL 1.1) at the three cell heights the
firmware uses (Small=11, Medium=16, Hero=25) into 1-bit bitmaps and emits
the exact data format doto_font.h has always exposed:

  - 95 glyphs, ASCII ' ' (0x20) .. '~' (0x7e), one DotoGlyph {code, advance,
    nibbleOffset} each;
  - a hex-nibble string; glyph (x, y) is bit  (y * advance + x)  of the glyph's
    block, packed MSB-first into nibbles, y = 0 is the TOP row;
  - dotoGlyphPixel() indexes the string at  nibbleOffset + bitIndex/4  as hex
    characters, so block size = ceil(advance * height / 4) nibbles.

Unlike Doto, Inter is proportional: advances are per glyph (the accessor API
already supports that - dotoGlyphAdvance() is per character and every caller
steps the cursor by it). Letters and digits share a baseline per tier; unusually
tall punctuation is fitted separately so no glyph is cut off.

Usage:
    python3 tools/make_inter_font.py [--weight Bold] ...

The Inter TTF is taken from --ttf, else from the cache directory, else
downloaded from the pinned release URL (honours HTTP_PROXY/HTTPS_PROXY).

Dependencies: Pillow (PIL). Deterministic: same TTF + same flags produce
byte-identical output (no timestamps are written).
"""

from __future__ import annotations

import argparse
from functools import lru_cache
import hashlib
import os
import pathlib
import sys
import urllib.request
import zipfile

from PIL import Image, ImageDraw, ImageFont

FW_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_OUT = FW_ROOT / "src" / "doto_font.cpp"

INTER_RELEASE_URL = "https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip"
INTER_VERSION = "4.1"
UPSTREAM_URL = "https://github.com/rsms/inter"
LICENSE = "SIL Open Font License 1.1 (https://github.com/rsms/inter/blob/develop/LICENSE.txt)"

# Cell heights are shared by all UI pages.
TIERS = (("Small", 11), ("Medium", 16), ("Hero", 25))
CHARS = [chr(c) for c in range(0x20, 0x7F)]

# Letters and digits retain their common baseline; tall punctuation is fitted
# into the cell individually after the shared size has been chosen.
PRIMARY = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
              " !\"#%&'*,.:;?@_-+=")
SECONDARY = set("()[]{}$/\\<>")
FREE = set("|`^~")
assert PRIMARY | SECONDARY | FREE == set(CHARS), "every printable ASCII char must be classified"


def cache_dir() -> pathlib.Path:
    return pathlib.Path(os.environ.get("INTER_FONT_CACHE",
                                       str(pathlib.Path.home() / ".cache" / "rlcd-agent-pet" / "inter")))


def obtain_ttf(weight: str, ttf_override: str | None) -> pathlib.Path:
    """Return a local Inter static TTF for the requested weight."""
    name = f"Inter-{weight}.ttf"
    if ttf_override:
        path = pathlib.Path(ttf_override)
        if not path.is_file():
            raise SystemExit(f"--ttf path not found: {path}")
        return path
    cached = cache_dir() / name
    if cached.is_file():
        return cached

    cache = cache_dir()
    cache.mkdir(parents=True, exist_ok=True)
    zip_path = cache / f"Inter-{INTER_VERSION}.zip"
    if not zip_path.is_file():
        print(f"downloading {INTER_RELEASE_URL} (proxy env vars honoured) ...")
        request = urllib.request.Request(INTER_RELEASE_URL, headers={"User-Agent": "make_inter_font/1.0"})
        with urllib.request.urlopen(request, timeout=300) as response, zip_path.open("wb") as sink:
            sink.write(response.read())
    member = f"extras/ttf/{name}"
    with zipfile.ZipFile(zip_path) as archive:
        with archive.open(member) as source, cached.open("wb") as sink:
            sink.write(source.read())
    print(f"extracted {member} -> {cached}")
    return cached


class Glyph:
    __slots__ = ("ch", "advance", "rows")

    def __init__(self, ch: str, advance: int, rows: list[list[bool]]):
        self.ch = ch
        self.advance = advance
        self.rows = rows  # height rows of advance bools

    @property
    def ink_pixels(self) -> int:
        return sum(sum(1 for px in row if px) for row in self.rows)


@lru_cache(maxsize=None)
def render_glyphs(font_path: str, size: int, threshold: int | None) -> tuple[ImageFont.FreeTypeFont, dict[str, tuple]]:
    """Render all chars at `size`; return font + per-char ink boxes relative to
    the baseline origin: (ink_left, ink_top, ink_right_excl, ink_bottom_excl)
    measured on the monochrome bitmap (top < 0 above the baseline)."""
    font = ImageFont.truetype(font_path, size)
    pad = size * 2
    base = size * 2
    canvas = Image.new("L", (pad * 3, base * 2 + size), 0)
    draw = ImageDraw.Draw(canvas)
    draw.fontmode = "1" if threshold is None else "L"
    boxes: dict[str, tuple] = {}
    for ch in CHARS:
        canvas.paste(0, (0, 0, canvas.width, canvas.height))
        draw.text((pad, base), ch, font=font, fill=255, anchor="ls")
        binary = canvas if threshold is None else canvas.point(lambda p, t=threshold: 255 if p >= t else 0)
        bbox = binary.getbbox()
        if bbox is None:
            boxes[ch] = None  # no ink (space)
            continue
        left, top, right, bottom = bbox
        boxes[ch] = (left - pad, top - base, right - pad, bottom - base)
    return font, boxes


def build_bitmap(font: ImageFont.FreeTypeFont, ch: str, baseline_row: int, threshold: int | None,
                 advance: int, height: int, pen_shift: int = 0) -> Glyph:
    """Render one glyph with its baseline on canvas row (headroom + baseline_row)
    and crop to an advance x height cell. Cell row 0 is `baseline_row` px above
    the baseline, so caps land near row 0 (the original data's convention)."""
    headroom = baseline_row + 8  # room for glyphs that overshoot above caps
    pad = advance + 8
    canvas = Image.new("L", (pad * 2 + 4, headroom + height + 8), 0)
    draw = ImageDraw.Draw(canvas)
    draw.fontmode = "1" if threshold is None else "L"
    draw.text((pad + pen_shift, headroom + baseline_row), ch, font=font, fill=255, anchor="ls")
    binary = canvas if threshold is None else canvas.point(lambda p, t=threshold: 255 if p >= t else 0)
    rows = []
    for y in range(height):
        row = []
        for x in range(advance):
            row.append(binary.getpixel((pad + x, headroom + y)) != 0)
        rows.append(row)
    return Glyph(ch, advance, rows)


def fit_tier(label: str, height: int, font_path: str, args) -> dict:
    """Pick a shared size and baseline, then fit taller punctuation to the cell."""
    best = None
    required_caps = int(args.fill * height + 0.999)  # ceil
    for size in range(height * 2, 4, -1):
        _, boxes = render_glyphs(font_path, size, args.threshold)
        cap_box = boxes["H"]
        cap_height = -cap_box[1]
        if cap_height < required_caps:  # caps must reach the fill target
            continue
        if cap_height > height - 2:
            continue

        for baseline in range(max(2, cap_height - 2), min(height, cap_height + 4)):
            soft_clip = 0
            core_ok = True
            for ch in CHARS:
                box = boxes[ch]
                if box is None:
                    continue
                cut_top = max(0, -(baseline + box[1]))
                cut_bottom = max(0, baseline + box[3] - height)
                if cut_top or cut_bottom:
                    if ch in SECONDARY:
                        soft_clip += cut_top + cut_bottom
                    elif ch in PRIMARY:
                        top_limit = 0 if (ch.isupper() or ch.isdigit()) else args.allow_primary_top_clip
                        if cut_top > top_limit or cut_bottom > args.allow_core_bottom_clip:
                            core_ok = False
            if not core_ok or soft_clip > args.max_soft_clip:
                continue
            score = (size, -soft_clip, cap_height)
            if best is None or score > best["score"]:
                best = {
                    "score": score,
                    "size": size,
                    "baseline": baseline,
                    "cap_height": cap_height,
                    "boxes": boxes,
                }
            break  # first valid baseline for this size is the most top-aligned
        if best is not None:
            break  # largest valid size wins; stop descending

    if best is None:
        raise SystemExit(f"{label}: no size/baseline satisfies the constraints "
                         f"(fill={args.fill}, max_soft_clip={args.max_soft_clip}, "
                         f"allow_core_bottom_clip={args.allow_core_bottom_clip})")

    font, _ = render_glyphs(font_path, best["size"], args.threshold)
    glyphs: list[Glyph] = []
    adjusted = []
    for ch in CHARS:
        box = best["boxes"][ch]
        if box is None:
            # Blank glyph (space): natural proportional advance.
            advance = max(2, round(best["size"] * args.space_ratio))
            glyphs.append(Glyph(ch, advance, [[False] * advance for _ in range(height)]))
            continue
        glyph_size = best["size"]
        glyph_font = font
        while box[3] - box[1] > height:
            glyph_size -= 1
            if glyph_size < 4:
                raise SystemExit(f"{label}: cannot fit {ch!r} in {height} pixels")
            glyph_font, smaller_boxes = render_glyphs(font_path, glyph_size, args.threshold)
            box = smaller_boxes[ch]
            if box is None:
                raise SystemExit(f"{label}: {ch!r} disappeared while fitting")
        baseline = max(-box[1], min(best["baseline"], height - box[3]))
        pen_shift = max(0, -box[0])
        advance = pen_shift + box[2] + 1
        assert 0 <= baseline + box[1] and baseline + box[3] <= height
        assert 0 <= pen_shift + box[0] and pen_shift + box[2] <= advance
        glyph = build_bitmap(glyph_font, ch, baseline, args.threshold, advance, height, pen_shift=pen_shift)
        assert glyph.ink_pixels > 0, f"{label}: {ch!r} has no ink"
        glyphs.append(glyph)
        if glyph_size != best["size"] or baseline != best["baseline"]:
            adjusted.append(ch)

    print(f"{label}: size={best['size']} baseline={best['baseline']} caps={best['cap_height']}px "
          f"of {height}px ({best['cap_height'] * 100 // height}%) "
          f"advances={min(g.advance for g in glyphs)}..{max(g.advance for g in glyphs)}; "
          f"individually fitted: {''.join(adjusted) or 'none'}")
    return {"glyphs": glyphs, "height": height, "size": best["size"], "baseline": best["baseline"]}


def pack_nibbles(glyph: Glyph, height: int) -> str:
    nibbles = ["0"] * (-(-glyph.advance * height // 4))
    for y in range(height):
        for x in range(glyph.advance):
            if glyph.rows[y][x]:
                index = y * glyph.advance + x
                nibbles[index // 4] = format(int(nibbles[index // 4], 16) | (1 << (3 - index % 4)), "X")
    return "".join(nibbles)


def c_char(ch: str) -> str:
    if ch == "\\":
        return "'\\\\'"
    if ch == "'":
        return "'\\''"
    return f"'{ch}'"


CPP_TAIL = r"""
uint8_t hexValue(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<uint8_t>(value - '0');
  }
  if (value >= 'A' && value <= 'F') {
    return static_cast<uint8_t>(value - 'A' + 10);
  }
  return 0;
}

const DotoFont& fontForSize(DotoFontSize size) {
  switch (size) {
    case DotoFontSize::Hero:
      return kDotoHeroFont;
    case DotoFontSize::Medium:
      return kDotoMediumFont;
    case DotoFontSize::Small:
    default:
      return kDotoSmallFont;
  }
}

const DotoGlyph* findGlyph(const DotoFont& font, char ch) {
  const char wanted = (ch >= ' ' && ch <= '~') ? ch : '?';
  for (uint8_t index = 0; index < font.glyphCount; ++index) {
    if (font.glyphs[index].code == wanted) {
      return &font.glyphs[index];
    }
  }
  return nullptr;
}

}  // namespace

int dotoTextWidth(DotoFontSize size, const char* text) {
  if (text == nullptr || text[0] == 0) {
    return 0;
  }
  const DotoFont& font = fontForSize(size);
  int width = 0;
  for (const char* cursor = text; *cursor != 0; ++cursor) {
    const DotoGlyph* glyph = findGlyph(font, *cursor);
    if (glyph != nullptr) {
      width += glyph->advance;
    }
  }
  return width;
}

int dotoTextHeight(DotoFontSize size) {
  return fontForSize(size).height;
}

int dotoGlyphAdvance(DotoFontSize size, char ch) {
  const DotoFont& font = fontForSize(size);
  const DotoGlyph* glyph = findGlyph(font, ch);
  return glyph == nullptr ? 0 : glyph->advance;
}

bool dotoGlyphPixel(DotoFontSize size, char ch, uint8_t x, uint8_t y) {
  const DotoFont& font = fontForSize(size);
  const DotoGlyph* glyph = findGlyph(font, ch);
  if (glyph == nullptr || x >= glyph->advance || y >= font.height) {
    return false;
  }
  const uint16_t bitIndex = static_cast<uint16_t>(y) * glyph->advance + x;
  const uint8_t nibble = hexValue(font.bits[glyph->nibbleOffset + bitIndex / 4]);
  return (nibble & (1 << (3 - (bitIndex % 4)))) != 0;
}
"""


def emit_cpp(tiers: dict, ttf_path: pathlib.Path, args) -> str:
    ttf_sha = hashlib.sha256(ttf_path.read_bytes()).hexdigest()
    lines = []
    lines.append('#include "doto_font.h"')
    lines.append("#include <stddef.h>")
    lines.append("")
    lines.append("namespace {")
    lines.append("")
    lines.append("// Bitmap glyphs rasterized from Inter (proportional sans by Rasmus Andersson)")
    lines.append(f"// Source font: Inter {INTER_VERSION}, {UPSTREAM_URL} (releases/download/v{INTER_VERSION})")
    lines.append(f"// License: {LICENSE}")
    lines.append("// Generated by tools/make_inter_font.py -- DO NOT EDIT BY HAND; rerun the script instead.")
    lines.append(f"// TTF: {ttf_path.name} sha256={ttf_sha}")
    mode = "monochrome" if args.threshold is None else f"threshold={args.threshold}"
    lines.append(f"// Params: weight={args.weight} rendering={mode} fill={args.fill} "
                 f"space_ratio={args.space_ratio} max_soft_clip={args.max_soft_clip} "
                 f"allow_primary_top_clip={args.allow_primary_top_clip} "
                 f"allow_core_bottom_clip={args.allow_core_bottom_clip}")
    for label, height in TIERS:
        tier = tiers[label]
        advances = sorted({g.advance for g in tier["glyphs"]})
        lines.append(f"// {label}: cell height={height}px, font px size={tier['size']}, baseline row={tier['baseline']}, "
                     f"proportional advances {advances[0]}..{advances[-1]}px")
    lines.append("")
    lines.append("struct DotoGlyph {")
    lines.append("  char code;")
    lines.append("  uint8_t advance;")
    lines.append("  uint16_t nibbleOffset;")
    lines.append("};")
    lines.append("")
    lines.append("struct DotoFont {")
    lines.append("  uint8_t height;")
    lines.append("  const DotoGlyph* glyphs;")
    lines.append("  uint8_t glyphCount;")
    lines.append("  const char* bits;")
    lines.append("};")
    for label, height in TIERS:
        tier = tiers[label]
        chunks = []
        for glyph in tier["glyphs"]:
            chunks.append(pack_nibbles(glyph, height))
        bits = "".join(chunks)
        lines.append("")
        lines.append(f"const char kDoto{label}Bits[] =")
        for i in range(0, len(bits), 70):
            lines.append(f'    "{bits[i:i + 70]}"')
        lines.append("    ;")
    for label, height in TIERS:
        tier = tiers[label]
        lines.append("")
        lines.append(f"const DotoGlyph kDoto{label}Glyphs[] = {{")
        offset = 0
        for glyph in tier["glyphs"]:
            lines.append(f"    {{{c_char(glyph.ch)}, {glyph.advance}, {offset}}},")
            offset += -(-glyph.advance * height // 4)
        lines.append("};")
    lines.append("")
    lines.append("const DotoFont kDotoSmallFont{11, kDotoSmallGlyphs, "
                 "static_cast<uint8_t>(sizeof(kDotoSmallGlyphs) / sizeof(kDotoSmallGlyphs[0])), kDotoSmallBits};")
    lines.append("const DotoFont kDotoMediumFont{16, kDotoMediumGlyphs, "
                 "static_cast<uint8_t>(sizeof(kDotoMediumGlyphs) / sizeof(kDotoMediumGlyphs[0])), kDotoMediumBits};")
    lines.append("const DotoFont kDotoHeroFont{25, kDotoHeroGlyphs, "
                 "static_cast<uint8_t>(sizeof(kDotoHeroGlyphs) / sizeof(kDotoHeroGlyphs[0])), kDotoHeroBits};")
    lines.append(CPP_TAIL)
    text = "\n".join(lines)
    if not text.endswith("\n"):
        text += "\n"
    # Collapse triple blank lines that can arise between generated sections.
    text = text.replace("\n\n\n", "\n\n")
    return text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ttf", help="path to a static Inter TTF (default: cache or pinned release download)")
    parser.add_argument("--weight", default="Bold", choices=["Medium", "SemiBold", "Bold"],
                        help="Inter static weight (default Bold: 1-bit reflective panels need heavy stems)")
    parser.add_argument("--out", default=str(DEFAULT_OUT), help="output .cpp path")
    parser.add_argument("--threshold", type=int,
                        help="use grayscale thresholding instead of FreeType's monochrome rendering")
    parser.add_argument("--fill", type=float, default=0.70,
                        help="minimum cap-height fraction of the cell height (default 0.70)")
    parser.add_argument("--space-ratio", type=float, default=0.30,
                        help="space glyph advance as a fraction of the font px size (default 0.30)")
    parser.add_argument("--max-soft-clip", type=int, default=40,
                        help="total off-cell rows allowed for secondary glyphs before fitting (default 40)")
    parser.add_argument("--allow-primary-top-clip", type=int, default=0,
                        help="rows a lowercase ascender may initially extend above the cell; "
                             "digit/cap tops never clip (default 0)")
    parser.add_argument("--allow-core-bottom-clip", type=int, default=0,
                        help="rows a primary glyph may initially extend below the cell (default 0)")
    args = parser.parse_args()

    ttf_path = obtain_ttf(args.weight, args.ttf)
    print(f"source ttf: {ttf_path} (sha256 {hashlib.sha256(ttf_path.read_bytes()).hexdigest()[:16]}...)")

    tiers = {}
    for label, height in TIERS:
        tiers[label] = fit_tier(label, height, str(ttf_path), args)

    cpp = emit_cpp(tiers, ttf_path, args)
    out = pathlib.Path(args.out)
    out.write_text(cpp)
    print(f"wrote {out} ({len(cpp)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
