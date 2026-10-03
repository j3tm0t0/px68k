#!/usr/bin/env python3
"""Build a substitute X68000 CG ROM image (cgrom.dat, 768 KB) from free fonts.

  tools/mkcgrom.py [out]        (default roms/cgrom.dat)

The layout follows make_cgromdat() in x11/mkcgrom.c (keropi's Windows
generator), and the control-code glyphs, box-drawing glyphs and 8x8 font are
keropi's tables parsed out of that file. The rest comes from bitmap fonts that
are downloaded once into tools/fonts-cache/:

  Shinonome 0.9.11 (The Electronic Font Open Laboratory) -- public domain
    https://deb.debian.org/debian/pool/main/x/xfonts-shinonome/xfonts-shinonome_0.9.11.orig.tar.gz
    shnmk16.bdf  16x16 JIS X 0208      shnmk12.bdf  12x12 JIS X 0208
    shnm8x16r.bdf 8x16 JIS X 0201 (roman + katakana)
  X.Org font-jis-misc 1.0.4 -- "from JIS X 9052-1983, by permission to use"
    https://www.x.org/releases/individual/font/font-jis-misc-1.0.4.tar.xz
    jiskan24.bdf 24x24 JIS X 0208
  X.Org font-sony-misc 1.0.4 -- Copyright 1989 Sony Corp., MIT-style permission
    https://www.x.org/releases/individual/font/font-sony-misc-1.0.4.tar.xz
    12x24rk.bdf  12x24 JIS X 0201 (roman + katakana)

CG ROM layout (offsets in the 0xF00000 window; the emulator reads the file
byte for byte, rows are stored MSB = leftmost dot):
  0x00000  16x16 full width, 32 bytes/char, 0x5E chars per JIS row:
           rows 0x21-0x27 -> lines 0-6, line 7 = box drawing, 0x30-0x74 -> lines 8-0x4C
  0x3A000  8x8   half width, 8 bytes/char, codes 0x00-0xFF
  0x3A800  8x16  half width, 16 bytes/char
  0x3B800  12x12 half width codes drawn with full width glyphs, 24 bytes/char
           (12 dots left-aligned in 16-bit rows)
  0x3D000  12x24 half width, 48 bytes/char (same row format)
  0x40000  24x24 full width, 72 bytes/char, same line arrangement as 16x16
"""
import os
import re
import sys
import tarfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "tools", "fonts-cache")
MKCGROM_C = os.path.join(ROOT, "x11", "mkcgrom.c")

ARCHIVES = {
    "xfonts-shinonome-0.9.11": "https://deb.debian.org/debian/pool/main/x/xfonts-shinonome/"
                               "xfonts-shinonome_0.9.11.orig.tar.gz",
    "font-jis-misc-1.0.4": "https://www.x.org/releases/individual/font/font-jis-misc-1.0.4.tar.xz",
    "font-sony-misc-1.0.4": "https://www.x.org/releases/individual/font/font-sony-misc-1.0.4.tar.xz",
}
FONTS = {
    "k16": "xfonts-shinonome-0.9.11/bdf/shnmk16.bdf",
    "k12": "xfonts-shinonome-0.9.11/bdf/shnmk12.bdf",
    "a8x16": "xfonts-shinonome-0.9.11/bdf/shnm8x16r.bdf",
    "k24": "font-jis-misc-1.0.4/jiskan24.bdf",
    "a12x24": "font-sony-misc-1.0.4/12x24rk.bdf",
}

SIZE = 0xC0000
OFS_8x8, OFS_8x16, OFS_12x12, OFS_12x24, OFS_24x24 = 0x3A000, 0x3A800, 0x3B800, 0x3D000, 0x40000


def fetch_fonts():
    os.makedirs(CACHE, exist_ok=True)
    for name, url in ARCHIVES.items():
        if os.path.isdir(os.path.join(CACHE, name)):
            continue
        path = os.path.join(CACHE, os.path.basename(url))
        if not os.path.exists(path):
            print(f"download {url}")
            urllib.request.urlretrieve(url, path)
        with tarfile.open(path) as t:
            t.extractall(CACHE, filter="data")


class Bdf:
    """Glyphs of a BDF font rendered into its full cell (width x height)."""

    def __init__(self, path):
        self.glyphs = {}
        with open(path, encoding="latin-1") as f:
            lines = f.read().splitlines()
        props = {}
        for ln in lines:
            k, _, v = ln.partition(" ")
            if k in ("FONTBOUNDINGBOX", "FONT_ASCENT", "FONT_DESCENT"):
                props[k] = v
            if k == "STARTCHAR":
                break
        fw, fh, fx, fy = map(int, props["FONTBOUNDINGBOX"].split())
        self.ascent = int(props.get("FONT_ASCENT", fh + fy))
        self.height = self.ascent + int(props.get("FONT_DESCENT", -fy))
        self.width = fw
        self.xoff = fx
        i = 0
        while i < len(lines):
            if not lines[i].startswith("STARTCHAR"):
                i += 1
                continue
            enc = None
            while not lines[i].startswith("BITMAP"):
                k, _, v = lines[i].partition(" ")
                if k == "ENCODING":
                    enc = int(v.split()[0])
                elif k == "BBX":
                    w, h, x, y = map(int, v.split())
                i += 1
            i += 1
            rows = []
            while not lines[i].startswith("ENDCHAR"):
                rows.append(int(lines[i], 16))
                i += 1
            nbits = ((w + 7) // 8) * 8
            cell = [0] * self.height
            top = self.ascent - (y + h)
            for r, bits in enumerate(rows):
                yy = top + r
                if 0 <= yy < self.height:
                    # Align to the cell: column 0 = bit (width - 1).
                    shift = self.width - (x - self.xoff) - nbits
                    v = bits << shift if shift >= 0 else bits >> -shift
                    cell[yy] = v & ((1 << self.width) - 1)
            if enc is not None and enc >= 0:
                self.glyphs[enc] = cell
            i += 1

    def get(self, code):
        return self.glyphs.get(code)


def parse_tables():
    """keropi's tables from x11/mkcgrom.c, as bytes in memory (little-endian WORDs)."""
    src = open(MKCGROM_C, "rb").read().decode("euc_jp")
    src_nc = re.sub(r"//[^\n]*", "", src)
    tables = {}
    for m in re.finditer(r"static (BYTE|WORD) (\w+)\[\] = \{(.*?)\};", src_nc, re.S):
        vals = [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]+", m.group(3))]
        if m.group(1) == "BYTE":
            tables[m.group(2)] = bytes(vals)
        else:  # memcpy'd from WORD arrays on little-endian Windows
            tables[m.group(2)] = b"".join(v.to_bytes(2, "little") for v in vals)
    m = re.search(r"static char \*str_x68k\[14\] = \{(.*?)\};", src, re.S)
    tables["str_x68k"] = re.findall(r'"([^"]*)"', m.group(1))
    m = re.search(r"static int deltable\[\] = \{(.*?)\};", src_nc, re.S)
    vals = [int(v, 16) if v.startswith("0x") else int(v)
            for v in re.findall(r"0x[0-9A-Fa-f]+|\b0\b", m.group(1))]
    rows, cur = [], []
    for v in vals:
        if v == 0:
            rows.append(cur)
            cur = []
        else:
            cur.append(v)
    tables["deltable"] = rows  # for JIS rows 0x22-0x27: [start, end) pairs of column-0x20
    return tables


def jis_of(ch):
    """JIS X 0208 code of a full width character."""
    b = ch.encode("euc_jp")
    return ((b[0] & 0x7F) << 8) | (b[1] & 0x7F)


def rows_bytes(cell, width, row_bytes):
    """Cell rows as big-endian bytes, left-aligned in row_bytes."""
    out = bytearray()
    for v in cell:
        out += (v << (row_bytes * 8 - width)).to_bytes(row_bytes, "big")
    return bytes(out)


def squeeze(cell, width):
    """Halve the width of a glyph (OR of neighbouring dots)."""
    out = []
    for v in cell:
        n = 0
        for x in range(width // 2):
            pair = (v >> (width - 2 - 2 * x)) & 3
            n = (n << 1) | (1 if pair else 0)
        out.append(n)
    return out


def blanked(row, col, deltable):
    """Codes keropi's generator leaves empty (not in JIS C 6226-1978 / the X68000 ROM)."""
    i = col - 0x20
    if 0x22 <= row <= 0x27:
        pairs = deltable[row - 0x22]
        for a, b in zip(pairs[0::2], pairs[1::2]):
            if a <= i < b:
                return True
    return (row == 0x4F and i >= 0x34) or (row == 0x74 and i >= 0x05)


def kanji_lines():
    """(line, JIS row) of the full width areas; line 7 holds the box drawing table."""
    return [(r - 0x21, r) for r in range(0x21, 0x28)] + [(r - 0x28, r) for r in range(0x30, 0x75)]


def build():
    fetch_fonts()
    f = {k: Bdf(os.path.join(CACHE, p)) for k, p in FONTS.items()}
    t = parse_tables()
    buf = bytearray(SIZE)

    def put(ofs, data):
        buf[ofs:ofs + len(data)] = data

    # 8x8: keropi's table for codes 0x01-0xFD.
    put(OFS_8x8 + 0x01 * 8, t["x68c8"][:0xFD * 8])

    # 8x16: font for 0x21-0x7E and 0xA1-0xDF, keropi's glyphs for the rest.
    for c in list(range(0x21, 0x7F)) + list(range(0xA1, 0xE0)):
        g = f["a8x16"].get(c)
        if g:
            put(OFS_8x16 + c * 16, rows_bytes(g, 8, 1))
    c16 = t["x68c16"]
    for code, word, n in ((0x01, 0x00, 0x1F), (0x80, 0x1F, 0x03), (0x86, 0x22, 0x0A),
                          (0x91, 0x2C, 0x0F), (0xE0, 0x3B, 0x1E)):
        put(OFS_8x16 + code * 16, c16[word * 16:(word + n) * 16])

    # 12x12: codes 0x20-0xFF drawn with the full width characters of str_x68k.
    for i, s in enumerate(t["str_x68k"]):
        for k, ch in enumerate(s):
            g = f["k12"].get(jis_of(ch))
            if g and ch != "　":
                put(OFS_12x12 + (0x20 + i * 16 + k) * 24, rows_bytes(g, 12, 2))
    c12 = t["x68c12"]
    put(OFS_12x12 + 0x01 * 24, c12[:0x1F * 24])
    put(OFS_12x12 + 0x80 * 24, c12[0x1F * 24:0x22 * 24])

    # 12x24: ANK and katakana from the font, hiragana (0x80-0x9F, 0xE0-0xFF)
    # from the 24 dot kanji font squeezed to half width.
    for c in list(range(0x21, 0x7F)) + list(range(0xA1, 0xE0)):
        g = f["a12x24"].get(c)
        if g:
            put(OFS_12x24 + c * 48, rows_bytes(g, 12, 2))
    for row, code in ((6, 0x80), (7, 0x90), (12, 0xE0), (13, 0xF0)):
        for k, ch in enumerate(t["str_x68k"][row]):
            g = f["k24"].get(jis_of(ch)) if ch != "　" else None
            if g:
                put(OFS_12x24 + (code + k) * 48, rows_bytes(squeeze(g, 24), 12, 2))
    c24 = t["x68c24"]
    put(OFS_12x24 + 0x01 * 48, c24[:0x1F * 48])
    put(OFS_12x24 + 0x80 * 48, c24[0x1F * 48:0x22 * 48])

    # 16x16 and 24x24 full width.
    for area, font, w, size, box in ((0, f["k16"], 16, 32, "x68k16"),
                                     (OFS_24x24, f["k24"], 24, 72, "x68k24")):
        for line, row in kanji_lines():
            for col in range(0x21, 0x7F):
                if blanked(row, col, t["deltable"]):
                    continue
                g = font.get((row << 8) | col)
                if g:
                    put(area + (line * 0x5E + col - 0x21) * size, rows_bytes(g, w, w // 8))
        put(area + 0x5E * 7 * size, t[box][:32 * size])

    return bytes(buf)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "roms", "cgrom.dat")
    data = build()
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "wb") as fp:
        fp.write(data)
    print(f"{out}: {len(data)} bytes")


if __name__ == "__main__":
    main()
