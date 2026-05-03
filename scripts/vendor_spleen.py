#!/usr/bin/env python3
"""
Vendor Spleen bitmap font family (5x8, 6x12, 8x16) as bb_display C sources.
Downloads BDF files, parses glyphs, and emits row-major MSB-first C arrays.
"""

import urllib.request
import re
from pathlib import Path

FONTS = [
    ("spleen-5x8.bdf", 5, 8, "bb_display_font_5x8.c"),
    ("spleen-6x12.bdf", 6, 12, "bb_display_font_6x12.c"),
    ("spleen-8x16.bdf", 8, 16, "bb_display_font_8x16.c"),
]

BASE_URL = "https://github.com/fcambus/spleen/raw/master/"
LICENSE_URL = BASE_URL + "LICENSE"


def download_bdf(filename):
    """Download BDF from GitHub."""
    url = BASE_URL + filename
    print(f"Downloading {filename}...")
    with urllib.request.urlopen(url) as response:
        return response.read().decode('utf-8')


def parse_bdf(bdf_text, nominal_w, nominal_h):
    """
    Parse BDF and extract glyphs 0x20..0x7E plus FONTBOUNDINGBOX yoff.
    Returns (glyphs_dict, font_yoff) where glyphs_dict maps:
      codepoint -> (bitmap_rows, bbx_w, bbx_h, bbx_xoff, bbx_yoff)
    """
    glyphs = {}
    font_yoff = 0
    lines = bdf_text.split('\n')
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if line.startswith('FONTBOUNDINGBOX'):
            parts = line.split()
            font_yoff = int(parts[4])

        # Find glyph start
        if line.startswith('STARTCHAR'):
            char_name = line.split(None, 1)[1] if len(line.split()) > 1 else None
            i += 1

            # Find ENCODING
            codepoint = None
            while i < len(lines):
                line = lines[i].strip()
                if line.startswith('ENCODING'):
                    codepoint = int(line.split()[1])
                    break
                i += 1

            if codepoint is None or codepoint < 0x20 or codepoint > 0x7E:
                i += 1
                continue

            i += 1

            # Find BBX
            bbx_w, bbx_h, bbx_xoff, bbx_yoff = None, None, None, None
            while i < len(lines):
                line = lines[i].strip()
                if line.startswith('BBX'):
                    parts = line.split()
                    bbx_w, bbx_h, bbx_xoff, bbx_yoff = int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4])
                    break
                i += 1

            if bbx_w is None:
                i += 1
                continue

            i += 1

            # Find BITMAP and read rows
            bitmap_rows = []
            while i < len(lines):
                line = lines[i].strip()
                if line == 'BITMAP':
                    i += 1
                    break
                i += 1

            # Read hex rows
            while i < len(lines):
                line = lines[i].strip()
                if line == 'ENDCHAR':
                    break
                if line and line != 'BITMAP':
                    # Parse hex line
                    hex_val = int(line, 16)
                    bitmap_rows.append(hex_val)
                i += 1

            # Validate and store
            if len(bitmap_rows) == bbx_h:
                glyphs[codepoint] = (bitmap_rows, bbx_w, bbx_h, bbx_xoff, bbx_yoff)

        i += 1

    return glyphs, font_yoff


def build_glyph_bitmap(bitmap_rows, bbx_w, bbx_h, bbx_xoff, bbx_yoff,
                       nominal_w, nominal_h, font_yoff):
    """
    Build the output bitmap for a glyph: nominal_h bytes, MSB-left, with
    columns 0..nominal_w-1 in bits 7..(8-nominal_w).

    BDF coords:
      - cell extends from y=font_yoff (bottom) to y=font_yoff+nominal_h-1 (top)
      - glyph bbx extends from y=bbx_yoff (bottom) to y=bbx_yoff+bbx_h-1 (top)
      - BITMAP rows are listed top-first
      - Each row's hex is MSB-left, holding bbx_w valid bits in the high byte
    Mapping bitmap row 0 (top) to cell row index:
      cell_row = (font_yoff + nominal_h - 1) - (bbx_yoff + bbx_h - 1)
               = font_yoff + nominal_h - bbx_yoff - bbx_h
    Then bitmap row k goes to cell_row + k.
    """
    output = [0] * nominal_h
    top_row = font_yoff + nominal_h - bbx_yoff - bbx_h

    # Mask off bits beyond nominal_w columns.
    width_mask = (0xFF << (8 - nominal_w)) & 0xFF if nominal_w < 8 else 0xFF

    for row_idx, hex_val in enumerate(bitmap_rows):
        # BDF rows are padded to byte boundary; high byte has bbx_w MSB-aligned bits.
        # Hex value width depends on bbx_w: 1 byte if bbx_w<=8, 2 bytes if 9..16, etc.
        byte_width = (bbx_w + 7) // 8
        hi_byte = (hex_val >> ((byte_width - 1) * 8)) & 0xFF
        # Shift right by xoff so column bbx_xoff lands at bit (7 - bbx_xoff).
        placed = (hi_byte >> bbx_xoff) & width_mask

        out_row = top_row + row_idx
        if 0 <= out_row < nominal_h:
            output[out_row] |= placed

    return output


def generate_c_file(font_name, glyph_w, glyph_h, bdf_text, output_path):
    """Generate a .c file for the font."""
    glyphs, font_yoff = parse_bdf(bdf_text, glyph_w, glyph_h)

    # Build array for codepoints 0x20..0x7E
    bitmap_data = []
    for cp in range(0x20, 0x7F):
        if cp in glyphs:
            bitmap_rows, bbx_w, bbx_h, bbx_xoff, bbx_yoff = glyphs[cp]
            glyph_bitmap = build_glyph_bitmap(bitmap_rows, bbx_w, bbx_h,
                                              bbx_xoff, bbx_yoff,
                                              glyph_w, glyph_h, font_yoff)
        else:
            glyph_bitmap = [0] * glyph_h
        bitmap_data.extend(glyph_bitmap)

    # Generate C code
    lines = [
        f"/* Spleen {glyph_w}x{glyph_h} bitmap font.",
        " * Vendored from https://github.com/fcambus/spleen",
        " * License: BSD-2-Clause (see ../SPLEEN-LICENSE) */",
        "",
        "#include \"bb_display.h\"",
        "",
        f"static const uint8_t s_font_{glyph_w}x{glyph_h}_bitmap[95 * {glyph_h}] = {{",
    ]

    # Write bytes in rows by glyph
    for glyph_idx in range(95):
        cp = 0x20 + glyph_idx
        char_repr = chr(cp) if 32 <= cp < 127 and cp not in (ord('\\'), ord('"')) else '?'
        if cp == ord('\\'):
            char_repr = '\\\\'
        elif cp == ord('"'):
            char_repr = '\\"'

        lines.append(f"    /* 0x{cp:02x} '{char_repr}' */")

        start_idx = glyph_idx * glyph_h
        end_idx = start_idx + glyph_h
        row_bytes = bitmap_data[start_idx:end_idx]

        hex_strs = ', '.join(f'0x{b:02x}' for b in row_bytes)
        lines.append(f"    {hex_strs},")

    lines.append("};")
    lines.append("")
    lines.append(f"const bb_display_font_t bb_display_font_{glyph_w}x{glyph_h} = {{")
    lines.append(f"    .glyph_w = {glyph_w},")
    lines.append(f"    .glyph_h = {glyph_h},")
    lines.append(f"    .first_codepoint = 0x20,")
    lines.append(f"    .glyph_count = 95,")
    lines.append(f"    .bitmap = s_font_{glyph_w}x{glyph_h}_bitmap,")
    lines.append("};")

    output_path.write_text('\n'.join(lines) + '\n')
    print(f"Wrote {output_path}")


def main():
    # Download license
    print("Downloading LICENSE...")
    license_text = urllib.request.urlopen(LICENSE_URL).read().decode('utf-8')

    # Write license file
    license_path = Path(__file__).parent.parent / "components" / "bb_display" / "SPLEEN-LICENSE"
    license_path.write_text(license_text)
    print(f"Wrote {license_path}")

    # Generate fonts
    base_path = Path(__file__).parent.parent / "components" / "bb_display" / "src"
    for bdf_filename, width, height, output_filename in FONTS:
        bdf_text = download_bdf(bdf_filename)
        output_path = base_path / output_filename
        generate_c_file(f"spleen_{width}x{height}", width, height, bdf_text, output_path)


if __name__ == '__main__':
    main()
