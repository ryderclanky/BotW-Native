"""Xbox-style button prompts for BotW Recompiled, drawn from the player's own game files.

  python tools/xbox_prompts.py --game <game folder> [--layout position|label] [--user <user folder>]
                               [--preview out.png] [--no-enable]

Every text prompt in the game (menus, dialogue, tutorials, the HUD's button text) is a private-use
character of the button font: External_00.bffnt in content/Font/Font_<region>.sbfarc, code points
from U+E040 (A, B, X, Y, L, R, ZL, ZR, Power, D-pad, Home, Plus, Minus, then the sticks). The font
is a bitmap font: one 128x1024 BC4 sheet in the GX2 2D_TILED_THIN1 layout, stored bottom-up, cells
of 42x39 with a 1-pixel border, two cells per row.

This tool redraws, in place and at the same size:
  L, R    -> LB, RB  (bumpers)
  ZL, ZR  -> LT, RT  (triggers)
  Plus    -> Menu    (three bars)
  Minus   -> View    (two overlapping windows)
and, with --layout position (the in-game Buttons setting "A = right"), swaps the A/B and X/Y
glyphs so each prompt names the label of the Xbox button that presses it. --layout label keeps
A/B/X/Y as they are.

The button guide in play and the action prompt draw their buttons from images instead:
timg/Nt_KeyTex{A,B,X,Y,L,ZL}_00^d.bflim in Layout/Common.sblarc, which is inside Pack/Bootup.pack
(48x48 A8, 2D_TILED_THIN1, each with its own bank/pipe swizzle). The same redraw and swap is
applied there (L -> LB, ZL -> LT; there are no R or ZR images), and Bootup.pack is written again
with the files after Common.sblarc moved by a multiple of 8 KiB so their alignment is kept.

The shapes and letters are drawn here; nothing of the game is in the repository. Every image keeps
its size and the glyph widths are unchanged, so the decompressed sizes of the font archive and
Common.sblarc are the same and the resource size table needs no change.

Output: <user>/mods/XboxPrompts/content/Font/<archive>, content/Pack/Bootup.pack (unless --no-hud)
and rules.txt, enabled in mods.ini (unless --no-enable). The user folder defaults to
BOTW_USER_ROOT or %APPDATA%/BotWRecomp.
"""
import argparse
import math
import os
import struct
import sys
import zlib
from pathlib import Path

MOD_NAME = "XboxPrompts"
FONT_NAME = "External_00.bffnt"

# Glyph indices in External_00.bffnt (U+E040 + index for the first 13).
G_A, G_B, G_X, G_Y, G_L, G_R, G_ZL, G_ZR = range(8)
G_PLUS, G_MINUS = 11, 12


# ---------------------------------------------------------------- Yaz0

def yaz0_tokens(data):
    """Decompressed bytes and the token list [(dst, kind, a, b)] of a Yaz0 stream.
    kind 0: literal byte a; kind 1: back reference, distance a, length b."""
    assert data[:4] == b"Yaz0", "not Yaz0"
    size = struct.unpack_from(">I", data, 4)[0]
    out = bytearray(size)
    toks = []
    src, dst = 16, 0
    while dst < size:
        code = data[src]
        src += 1
        for bit in range(8):
            if dst >= size:
                break
            if code & (0x80 >> bit):
                out[dst] = data[src]
                toks.append((dst, 0, data[src], 1))
                dst += 1
                src += 1
            else:
                b1, b2 = data[src], data[src + 1]
                src += 2
                dist = ((b1 & 0xF) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = data[src] + 0x12
                    src += 1
                else:
                    n += 2
                toks.append((dst, 1, dist, n))
                s = dst - dist
                if dist >= n:
                    out[dst:dst + n] = out[s:s + n]
                else:
                    for i in range(n):
                        out[dst + i] = out[s + i]
                dst += n
    return bytes(out), toks


def changed_ranges(old, new, base=0, merge=64):
    """Sorted [lo, hi) ranges (offset by base) where new differs from old, nearby ones merged."""
    out = []
    i, n = 0, len(old)
    while i < n:
        if old[i] != new[i]:
            j = i + 1
            while j < n and (old[j] != new[j] or any(old[k] != new[k] for k in range(j, min(j + merge, n)))):
                j += 1
            out.append((base + i, base + j))
            i = j
        else:
            i += 1
    return out


def yaz0_reencode(header, toks, new, ranges):
    """A Yaz0 stream that decompresses to `new`, which differs from the original only inside the
    sorted [lo, hi) ranges. Tokens that write into a range, or copy from one, become literals of
    the new bytes; the rest of the original stream is kept as it was."""
    import bisect
    los = [r[0] for r in ranges]

    def hits(a, b):
        k = bisect.bisect_right(los, b - 1) - 1
        return k >= 0 and ranges[k][1] > a

    ops = []
    for dst, kind, a, n in toks:
        end = dst + n
        touches = hits(dst, end)
        if kind == 1 and not touches:
            touches = hits(dst - a, dst - a + n)
        if not touches:
            ops.append((kind, a, n))
        else:
            ops.extend((0, new[p], 1) for p in range(dst, end))
    out = bytearray(header[:16])
    for g in range(0, len(ops), 8):
        group = ops[g:g + 8]
        code = 0
        body = bytearray()
        for i, (kind, a, n) in enumerate(group):
            if kind == 0:
                code |= 0x80 >> i
                body.append(a)
            else:
                d = a - 1
                if n >= 0x12:
                    body += bytes((d >> 8, d & 0xFF, n - 0x12))
                else:
                    body += bytes((((n - 2) << 4) | (d >> 8), d & 0xFF))
        out.append(code)
        out += body
    return bytes(out)


# ---------------------------------------------------------------- SARC

def _sarc_nodes(data):
    assert data[:4] == b"SARC", "not a SARC"
    e = ">" if data[6:8] == b"\xfe\xff" else "<"
    hdr_len, = struct.unpack_from(e + "H", data, 4)
    data_off, = struct.unpack_from(e + "I", data, 0x0C)
    count, = struct.unpack_from(e + "H", data, hdr_len + 6)
    names = hdr_len + 12 + count * 16 + 8
    nodes = []
    for i in range(count):
        at = hdr_len + 12 + i * 16
        _, attr, s, end = struct.unpack_from(e + "IIII", data, at)
        name = None
        if attr & 0x01000000:
            o = names + (attr & 0xFFFF) * 4
            name = data[o:data.index(b"\0", o)].decode()
        nodes.append((name, at, s, end))
    return e, data_off, nodes


def sarc_files(data, pred):
    """{name: (offset, size)} of the files of an uncompressed SARC whose names satisfy pred."""
    _, data_off, nodes = _sarc_nodes(data)
    return {n: (data_off + s, end - s) for n, _, s, end in nodes if n and pred(n)}


def sarc_find(data, want):
    """(offset, size) of the file named `want` inside an uncompressed SARC."""
    return sarc_files(data, lambda n: n == want).get(want)


def sarc_replace(data, want, blob, align=0x2000):
    """The SARC with file `want` replaced by blob. When blob is larger, every file stored after it
    moves by a multiple of `align`, so their alignment is kept."""
    e, data_off, nodes = _sarc_nodes(data)
    me = next(t for t in nodes if t[0] == want)
    _, at, s, end = me
    old = end - s
    if len(blob) <= old:
        out = bytearray(data)
        out[data_off + s:data_off + s + len(blob)] = blob
        out[data_off + s + len(blob):data_off + end] = bytes(old - len(blob))
        struct.pack_into(e + "I", out, at + 12, s + len(blob))
        return bytes(out)
    delta = (len(blob) - old + align - 1) // align * align
    out = bytearray(data[:data_off + s]) + blob + bytes(old + delta - len(blob)) + data[data_off + end:]
    for n, nat, ns, nend in nodes:
        if nat == at:
            struct.pack_into(e + "II", out, nat + 8, ns, ns + len(blob))
        elif ns >= end:
            struct.pack_into(e + "II", out, nat + 8, ns + delta, nend + delta)
    struct.pack_into(e + "I", out, 8, len(out))
    return bytes(out)


# ---------------------------------------------------------------- GX2 tiling (2D_TILED_THIN1, 64-bit elements)

# Wii U: 2 pipes, 4 banks, 256-byte pipe interleave. bpp is bits per element (a pixel, or a 4x4
# block for BC formats); swizzle is the surface's bank/pipe swizzle (bits 8-10).

def _pixel_index(x, y, bpp):
    x0, x1, x2 = x & 1, (x >> 1) & 1, (x >> 2) & 1
    y0, y1, y2 = y & 1, (y >> 1) & 1, (y >> 2) & 1
    bits = {8: (x0, x1, x2, y1, y0, y2), 16: (x0, x1, x2, y0, y1, y2), 32: (x0, x1, y0, x2, y1, y2),
            64: (x0, y0, x1, x2, y1, y2)}.get(bpp, (y0, x0, x1, x2, y1, y2))
    return sum(b << i for i, b in enumerate(bits))


def _addr(x, y, bpp, pitch, swizzle=0):
    elem = (bpp * _pixel_index(x, y, bpp)) // 8
    pipe = ((y >> 3) ^ (x >> 3)) & 1
    bank = (((y // 32) ^ (x >> 3)) & 1) | ((((y // 16) ^ (x >> 4)) & 1) << 1)
    bank_pipe = ((pipe + 2 * bank) ^ (((swizzle >> 8) & 1) + 2 * ((swizzle >> 9) & 3))) % 8
    pipe, bank = bank_pipe % 2, bank_pipe // 2
    macro = ((x // 32) + (pitch // 32) * (y // 16)) * (bpp * 16 * 32 // 8)
    total = elem + (macro >> 3)
    return (bank << 9) | (pipe << 8) | (total & 0xFF) | ((total & ~0xFF) << 3)


def untile(data, w, h, bpp=64, pitch=None, swizzle=0):
    """Elements of a 2D_TILED_THIN1 surface in row order (w x h elements)."""
    n = bpp // 8
    pitch = pitch or w
    out = bytearray(w * h * n)
    for y in range(h):
        for x in range(w):
            a, l = _addr(x, y, bpp, pitch, swizzle), (y * w + x) * n
            out[l:l + n] = data[a:a + n]
    return out


def tile_into(dst, lin, w, h, bpp=64, pitch=None, swizzle=0):
    """Writes row-ordered elements into the tiled surface dst (padding bytes are kept)."""
    n = bpp // 8
    pitch = pitch or w
    for y in range(h):
        for x in range(w):
            a, l = _addr(x, y, bpp, pitch, swizzle), (y * w + x) * n
            dst[a:a + n] = lin[l:l + n]


# ---------------------------------------------------------------- BC4

def bc4_decode(lin, w, h):
    px = bytearray(w * h)
    bw = w // 4
    for by in range(h // 4):
        for bx in range(bw):
            o = (by * bw + bx) * 8
            r0, r1 = lin[o], lin[o + 1]
            if r0 > r1:
                pal = [r0, r1] + [((7 - i) * r0 + i * r1) // 7 for i in range(1, 7)]
            else:
                pal = [r0, r1] + [((5 - i) * r0 + i * r1) // 5 for i in range(1, 5)] + [0, 255]
            bits = int.from_bytes(lin[o + 2:o + 8], "little")
            for i in range(16):
                px[(by * 4 + i // 4) * w + bx * 4 + (i & 3)] = pal[(bits >> (3 * i)) & 7]
    return px


def bc4_encode_block(vals):
    mx, mn = max(vals), min(vals)
    if mx == mn:
        return bytes((mx, mn, 0, 0, 0, 0, 0, 0))
    pal = [mx, mn] + [((7 - i) * mx + i * mn) // 7 for i in range(1, 7)]
    bits = 0
    for i, v in enumerate(vals):
        best = min(range(8), key=lambda k: abs(pal[k] - v))
        bits |= best << (3 * i)
    return bytes((mx, mn)) + bits.to_bytes(6, "little")


# ---------------------------------------------------------------- drawing

class Canvas:
    """Coverage of a set of shapes, sampled 4x4 per pixel. A shape is a function (x, y) -> bool."""
    SS = 4

    def __init__(self, w, h):
        self.w, self.h = w, h

    def render(self, inside):
        px = bytearray(self.w * self.h)
        s = self.SS
        for y in range(self.h):
            for x in range(self.w):
                c = 0
                for j in range(s):
                    for i in range(s):
                        if inside(x + (i + 0.5) / s, y + (j + 0.5) / s):
                            c += 1
                px[y * self.w + x] = (c * 255 + (s * s) // 2) // (s * s)
        return px


def rbox(x0, y0, x1, y1, rtl, rtr=None, rbr=None, rbl=None):
    """Inside test of a box with per-corner radii."""
    rtr = rtl if rtr is None else rtr
    rbr = rtl if rbr is None else rbr
    rbl = rtl if rbl is None else rbl

    def f(x, y):
        if x < x0 or x > x1 or y < y0 or y > y1:
            return False
        for cx, cy, r, sx, sy in ((x0 + rtl, y0 + rtl, rtl, -1, -1), (x1 - rtr, y0 + rtr, rtr, 1, -1),
                                  (x1 - rbr, y1 - rbr, rbr, 1, 1), (x0 + rbl, y1 - rbl, rbl, -1, 1)):
            if r > 0 and (x - cx) * sx > 0 and (y - cy) * sy > 0:
                return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
        return True
    return f


def ring(outer, inner):
    return lambda x, y: outer(x, y) and not inner(x, y)


def bar(ax, ay, bx, by, w):
    """A straight stroke of width w from a to b with square ends."""
    dx, dy = bx - ax, by - ay
    L = math.hypot(dx, dy)
    ux, uy = dx / L, dy / L

    def f(x, y):
        px, py = x - ax, y - ay
        t = px * ux + py * uy
        return 0 <= t <= L and abs(px * -uy + py * ux) <= w / 2
    return f


def union(*fs):
    return lambda x, y: any(f(x, y) for f in fs)


def letter(ch, x, y, w, h, sw):
    """Bold block capitals L, B, R, T in the box (x, y, w, h) with stroke width sw."""
    if ch == "L":
        return union(rbox(x, y, x + sw, y + h, 0), rbox(x, y + h - sw, x + w, y + h, 0))
    if ch == "T":
        return union(rbox(x, y, x + w, y + sw, 0), rbox(x + w / 2 - sw / 2, y, x + w / 2 + sw / 2, y + h, 0))
    mid = y + h * 0.5
    if ch == "B":
        r = (h / 2 + sw / 2) / 2
        top = ring(rbox(x, y, x + w - 0.8, mid + sw / 2, 0, r, r, 0),
                   rbox(x + sw, y + sw, x + w - 0.8 - sw, mid - sw / 2, 0, max(r - sw, 0.1), max(r - sw, 0.1), 0))
        bot = ring(rbox(x, mid - sw / 2, x + w, y + h, 0, r, r, 0),
                   rbox(x + sw, mid + sw / 2, x + w - sw, y + h - sw, 0, max(r - sw, 0.1), max(r - sw, 0.1), 0))
        return union(rbox(x, y, x + sw, y + h, 0), top, bot)
    if ch == "R":
        bh = h * 0.56
        r = bh / 2
        bowl = ring(rbox(x, y, x + w, y + bh, 0, r, r, 0),
                    rbox(x + sw, y + sw, x + w - sw, y + bh - sw, 0, max(r - sw, 0.1), max(r - sw, 0.1), 0))
        leg = bar(x + w * 0.42, y + bh - sw * 0.6, x + w + sw * 0.15, y + h + sw * 0.5, sw * 1.05)
        clip = lambda f: (lambda px, py: py <= y + h and f(px, py))
        return union(rbox(x, y, x + sw, y + h, 0), bowl, clip(leg))
    raise ValueError(ch)


def word(text, cx, cy, h, sw, lw, gap):
    """The letters of text centred on (cx, cy), each lw wide and h tall."""
    total = len(text) * lw + (len(text) - 1) * gap
    x = cx - total / 2
    parts = []
    for ch in text:
        parts.append(letter(ch, x, cy - h / 2, lw, h, sw))
        x += lw + gap
    return union(*parts)


def bbox(px, w, h, thr=96):
    xs = [x for y in range(h) for x in range(w) if px[y * w + x] >= thr]
    ys = [y for y in range(h) for x in range(w) if px[y * w + x] >= thr]
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


def _word_in(text, box, frac, dy=0.0):
    """Two letters sized to the shape: cap height frac of its height, capped so they fit its width."""
    x0, y0, x1, y1 = box
    lh = (y1 - y0) * frac
    lh = min(lh, (x1 - x0) * 0.78 / (len(text) * 0.82 + (len(text) - 1) * 0.15))
    return word(text, (x0 + x1) / 2, (y0 + y1) / 2 + dy * lh, lh, lh * 0.26, lh * 0.82, lh * 0.15)


def draw_bumper(text, box, w, h):
    x0, y0, x1, y1 = box
    shape = rbox(x0, y0, x1, y1, (y1 - y0) * 0.26)
    hole = _word_in(text, box, 0.62)
    return Canvas(w, h).render(lambda x, y: shape(x, y) and not hole(x, y))


def draw_trigger(text, box, w, h):
    x0, y0, x1, y1 = box
    r = (y1 - y0) * 0.45
    shape = rbox(x0, y0, x1, y1, r, r, r * 0.38, r * 0.38)
    hole = _word_in(text, box, 0.56, dy=0.08)
    return Canvas(w, h).render(lambda x, y: shape(x, y) and not hole(x, y))


def draw_circle_icon(kind, box, w, h):
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    r = min(x1 - x0, y1 - y0) / 2
    disc = lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    s = r / 13.0  # the game's Plus/Minus circles are about 26 px across
    if kind == "menu":
        hw, t = 7.0 * s, 2.2 * s
        hole = union(*(rbox(cx - hw, cy + dy - t / 2, cx + hw, cy + dy + t / 2, t / 2) for dy in (-5 * s, 0, 5 * s)))
    else:  # view: a window behind and up-left of another
        t = 1.9 * s
        back = rbox(cx - 7.5 * s, cy - 6.5 * s, cx + 2.5 * s, cy + 1.5 * s, 1.5 * s)
        back_in = rbox(cx - 7.5 * s + t, cy - 6.5 * s + t, cx + 2.5 * s - t, cy + 1.5 * s - t, 0.5 * s)
        front = rbox(cx - 3.0 * s, cy - 2.0 * s, cx + 7.5 * s, cy + 6.5 * s, 1.5 * s)
        front_in = rbox(cx - 3.0 * s + t, cy - 2.0 * s + t, cx + 7.5 * s - t, cy + 6.5 * s - t, 0.5 * s)
        hole = lambda x, y: (back(x, y) and not back_in(x, y) and not front(x, y)) or (front(x, y) and not front_in(x, y))
    return Canvas(w, h).render(lambda x, y: disc(x, y) and not hole(x, y))


# ---------------------------------------------------------------- font patch

class Font:
    def __init__(self, data):
        self.d = bytearray(data)
        assert self.d[:4] == b"FFNT" and self.d[4:6] == b"\xfe\xff", "not a big-endian FFNT"
        off, self.blocks = struct.unpack_from(">H", self.d, 6)[0], {}
        while off + 8 <= len(self.d) and bytes(self.d[off:off + 4]).isalpha():
            self.blocks.setdefault(bytes(self.d[off:off + 4]), off)
            off += struct.unpack_from(">I", self.d, off + 4)[0]
        t = self.blocks[b"TGLP"]
        (self.cw, self.ch, nsheets, _, self.ssize, _, fmt, self.per_row, _, self.sw, self.sh,
         self.doff) = struct.unpack_from(">BBBBIHHHHHHI", self.d, t + 8)
        assert nsheets == 1 and fmt == 12, f"expected one BC4 sheet, got {nsheets} sheets of format {fmt}"
        c = self.blocks[b"CWDH"]
        first, last = struct.unpack_from(">HH", self.d, c + 8)
        self.widths = [struct.unpack_from(">bBB", self.d, c + 16 + i * 3) for i in range(last - first + 1)]
        self.wb, self.hb = self.sw // 4, self.sh // 4
        self.lin = untile(self.d[self.doff:self.doff + self.ssize], self.wb, self.hb, 64)
        self.px = bc4_decode(self.lin, self.sw, self.sh)
        self.dirty = set()

    def _cell_origin(self, i):
        # Upright coordinates of the cell's top-left pixel; the sheet is stored bottom-up.
        return (i % self.per_row) * (self.cw + 1) + 1, (i // self.per_row) * (self.ch + 1) + 1

    def get(self, i):
        x0, y0 = self._cell_origin(i)
        out = bytearray(self.cw * self.ch)
        for y in range(self.ch):
            row = (self.sh - 1 - (y0 + y)) * self.sw
            out[y * self.cw:(y + 1) * self.cw] = self.px[row + x0:row + x0 + self.cw]
        return out

    def put(self, i, cell):
        x0, y0 = self._cell_origin(i)
        for y in range(self.ch):
            sy = self.sh - 1 - (y0 + y)
            row = sy * self.sw
            self.px[row + x0:row + x0 + self.cw] = cell[y * self.cw:(y + 1) * self.cw]
            for x in range(x0, x0 + self.cw, 1):
                self.dirty.add((x // 4, sy // 4))

    def build(self):
        for bx, by in self.dirty:
            vals = [self.px[(by * 4 + j) * self.sw + bx * 4 + i] for j in range(4) for i in range(4)]
            o = (by * self.wb + bx) * 8
            self.lin[o:o + 8] = bc4_encode_block(vals)
        sheet = bytearray(self.ssize)
        tile_into(sheet, self.lin, self.wb, self.hb, 64)
        self.d[self.doff:self.doff + self.ssize] = sheet
        return bytes(self.d)


def patch_font(font, layout):
    cw, ch = font.cw, font.ch
    orig = {i: font.get(i) for i in range(13)}
    gw = lambda i: font.widths[i][1]
    if layout == "position":
        for a, b in ((G_A, G_B), (G_X, G_Y)):
            font.put(a, orig[b])
            font.put(b, orig[a])
    for i, text in ((G_L, "LB"), (G_R, "RB")):
        _, y0, _, y1 = bbox(orig[i], cw, ch)
        font.put(i, draw_bumper(text, (0.5, y0 + 0.5, gw(i) - 0.5, y1 - 0.5), cw, ch))
    for i, text in ((G_ZL, "LT"), (G_ZR, "RT")):
        _, y0, _, y1 = bbox(orig[i], cw, ch)
        font.put(i, draw_trigger(text, (0.5, y0 - 0.5, gw(i) - 0.5, y1 - 0.5), cw, ch))
    for i, kind in ((G_PLUS, "menu"), (G_MINUS, "view")):
        x0, y0, x1, y1 = bbox(orig[i], cw, ch, thr=1)
        font.put(i, draw_circle_icon(kind, (x0, y0, x1, y1), cw, ch))
    return orig


# ---------------------------------------------------------------- HUD button guide (Layout/Common.sblarc)

# The button guide in play draws these images instead of font text. R and ZR have no images.
HUD = {k: f"timg/Nt_KeyTex{k}_00^d.bflim" for k in ("A", "B", "X", "Y", "L", "ZL")}


class Flim:
    """A Wii U .bflim (footer at the end) holding an A8 2D_TILED_THIN1 image."""

    def __init__(self, blob):
        self.b = bytearray(blob)
        f = self.b[-0x28:]
        assert f[:4] == b"FLIM" and f[0x14:0x18] == b"imag", "not a BFLIM"
        self.w, self.h, _, fmt, st = struct.unpack_from(">HHHBB", f, 0x1C)
        self.size, = struct.unpack_from(">I", f, 0x24)
        assert fmt == 1 and st & 0x1F == 4, f"expected an A8 tiled image, got format {fmt} tile mode {st & 0x1F}"
        self.swizzle = ((st >> 5) & 7) << 8
        self.pitch = self.size // ((self.h + 15) // 16 * 16)

    def pixels(self):
        return untile(self.b, self.w, self.h, 8, self.pitch, self.swizzle)

    def with_pixels(self, px):
        out = bytearray(self.b)
        tile_into(out, px, self.w, self.h, 8, self.pitch, self.swizzle)
        return bytes(out)


def patch_hud(common, layout):
    """Common.sblarc's SARC with the guide's images redrawn (each image keeps its size), and the
    (name, original, new, w, h) of every image for the preview."""
    files = sarc_files(common, lambda n: n in HUD.values())
    missing = [k for k, n in HUD.items() if n not in files]
    if missing:
        raise SystemExit(f"Common.sblarc lacks the guide images for {', '.join(missing)}")
    flims = {k: Flim(common[files[n][0]:files[n][0] + files[n][1]]) for k, n in HUD.items()}
    orig = {k: f.pixels() for k, f in flims.items()}
    new = dict(orig)
    if layout == "position":
        new["A"], new["B"], new["X"], new["Y"] = orig["B"], orig["A"], orig["Y"], orig["X"]
    for k, text, draw in (("L", "LB", draw_bumper), ("ZL", "LT", draw_trigger)):
        f = flims[k]
        x0, y0, x1, y1 = bbox(orig[k], f.w, f.h, thr=64)
        new[k] = draw(text, (x0 + 0.5, y0 + 0.5, x1 - 0.5, y1 - 0.5), f.w, f.h)
    out = bytearray(common)
    shown = []
    for k, n in HUD.items():
        off, size = files[n]
        blob = flims[k].with_pixels(new[k])
        assert len(blob) == size
        out[off:off + size] = blob
        shown.append((k, orig[k], new[k], flims[k].w, flims[k].h))
    return bytes(out), [files[n] for n in HUD.values()], shown


def patch_bootup(path, layout):
    """Bootup.pack with Layout/Common.sblarc's guide images redrawn, and the preview images."""
    pack = path.read_bytes()
    hit = sarc_find(pack, "Layout/Common.sblarc")
    if not hit:
        raise SystemExit(f"{path} has no Layout/Common.sblarc")
    off, size = hit
    raw = pack[off:off + size]
    common, toks = yaz0_tokens(raw)
    new_common, regions, shown = patch_hud(common, layout)
    ranges = []
    for o, s in sorted(regions):
        ranges += changed_ranges(common[o:o + s], new_common[o:o + s], base=o)
    blob = yaz0_reencode(raw, toks, new_common, ranges)
    assert yaz0_tokens(blob)[0] == new_common, "Yaz0 round trip failed"
    new_pack = sarc_replace(pack, "Layout/Common.sblarc", blob)
    o2, s2 = sarc_find(new_pack, "Layout/Common.sblarc")
    assert new_pack[o2:o2 + s2] == blob
    return new_pack, shown, sum(hi - lo for lo, hi in ranges)


# ---------------------------------------------------------------- preview

def preview_png(path, pairs, scale=3):
    """pairs: (original, new, w, h) images, one row each, original on the left."""
    cw = max(p[2] for p in pairs)
    W = (cw * 2 + 6) * scale
    H = sum(p[3] + 2 for p in pairs) * scale
    img = bytearray(W * H)
    top = 0
    for o_px, n_px, w, h in pairs:
        for k, cell in enumerate((o_px, n_px)):
            for y in range(h):
                for x in range(w):
                    v = cell[y * w + x]
                    for yy in range(scale):
                        base = ((top + 1 + y) * scale + yy) * W + (k * (cw + 4) + 1 + x) * scale
                        img[base:base + scale] = bytes((v,)) * scale
        top += h + 2
    raw = b"".join(b"\0" + img[y * W:(y + 1) * W] for y in range(H))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 0, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


# ---------------------------------------------------------------- mod output

def default_user_root():
    if os.environ.get("BOTW_USER_ROOT"):
        return Path(os.environ["BOTW_USER_ROOT"])
    return Path(os.environ.get("APPDATA", Path.home())) / "BotWRecomp"


def layout_from_settings(user):
    """position for the Buttons setting "A = right" (layout=0), label otherwise."""
    try:
        for line in (user / "settings.ini").read_text(encoding="utf-8", errors="replace").splitlines():
            k, _, v = line.partition("=")
            if k.strip() == "layout":
                return "position" if v.strip() == "0" else "label"
    except OSError:
        pass
    return "label"


def enable_in_mods_ini(user):
    path = user / "mods.ini"
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError:
        lines = []
    sec, idx, enabled_at = None, None, None
    for n, line in enumerate(lines):
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            sec = s[1:-1].strip().lower()
            if sec == "enabled":
                enabled_at = n
            continue
        if sec == "enabled" and s.partition("=")[0].strip().lower() == MOD_NAME.lower():
            idx = n
    if idx is not None:
        lines[idx] = f"{MOD_NAME} = 1"
    elif enabled_at is not None:
        lines.insert(enabled_at + 1, f"{MOD_NAME} = 1")
    else:
        lines = ["[Enabled]", f"{MOD_NAME} = 1", ""] + lines
    path.write_text("\n".join(lines).rstrip("\n") + "\n", encoding="utf-8")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--game", required=True, type=Path, help="merged game folder (contains content/)")
    ap.add_argument("--user", type=Path, default=None, help="user folder (default BOTW_USER_ROOT or %%APPDATA%%/BotWRecomp)")
    ap.add_argument("--layout", choices=("position", "label"), default=None,
                    help="position: A/B and X/Y prompts swapped for the 'A = right' Buttons setting; "
                         "label: letters kept. Default: from settings.ini")
    ap.add_argument("--preview", type=Path, help="also write a PNG: original glyphs left, new glyphs right")
    ap.add_argument("--no-enable", action="store_true", help="do not touch mods.ini")
    ap.add_argument("--no-hud", action="store_true",
                    help="patch only the font, not the button guide's images in Bootup.pack")
    a = ap.parse_args(argv)

    user = a.user or default_user_root()
    layout = a.layout or layout_from_settings(user)
    font_dir = a.game / "content" / "Font"
    archives = sorted(font_dir.glob("Font_*.sbfarc")) if font_dir.is_dir() else []
    if not archives:
        sys.exit(f"no content/Font/Font_*.sbfarc under {a.game}")

    bootup = a.game / "content" / "Pack" / "Bootup.pack"
    if not a.no_hud and not bootup.is_file():
        sys.exit(f"no content/Pack/Bootup.pack under {a.game} (use --no-hud to patch only the font)")

    mod = user / "mods" / MOD_NAME
    out_dir = mod / "content" / "Font"
    out_dir.mkdir(parents=True, exist_ok=True)
    pairs = []
    done = 0
    for arc in archives:
        raw = arc.read_bytes()
        sarc, toks = yaz0_tokens(raw)
        hit = sarc_find(sarc, FONT_NAME)
        if not hit:
            print(f"{arc.name}: no {FONT_NAME}, skipped")
            continue
        off, size = hit
        font = Font(sarc[off:off + size])
        orig = patch_font(font, layout)
        new_font = font.build()
        assert len(new_font) == size
        new_sarc = sarc[:off] + new_font + sarc[off + size:]
        ranges = changed_ranges(sarc[off:off + size], new_font, base=off)
        out = yaz0_reencode(raw, toks, new_sarc, ranges)
        check, _ = yaz0_tokens(out)
        assert check == new_sarc, "Yaz0 round trip failed"
        (out_dir / arc.name).write_bytes(out)
        print(f"{arc.name}: {sum(h - l for l, h in ranges)} bytes of {FONT_NAME} rewritten, wrote {out_dir / arc.name}")
        if done == 0:
            pairs += [(orig[i], font.get(i), font.cw, font.ch) for i in range(13)]
        done += 1
    if not done:
        sys.exit("no font archive had " + FONT_NAME)

    pack_out = mod / "content" / "Pack" / "Bootup.pack"
    if a.no_hud:
        if pack_out.exists():
            pack_out.unlink()
    else:
        new_pack, shown, nbytes = patch_bootup(bootup, layout)
        pack_out.parent.mkdir(parents=True, exist_ok=True)
        pack_out.write_bytes(new_pack)
        print(f"Bootup.pack: {nbytes} bytes of Layout/Common.sblarc rewritten "
              f"({', '.join(k for k, *_ in shown)}), wrote {pack_out}")
        pairs += [(o, n, w, h) for _, o, n, w, h in shown]
    if a.preview:
        preview_png(a.preview, pairs)
        print(f"preview: {a.preview}")

    (mod / "rules.txt").write_text(
        "[Definition]\n"
        "titleIds = 00050000101C9300,00050000101C9400,00050000101C9500\n"
        f"name = Xbox button prompts ({'A = right' if layout == 'position' else 'A = bottom'})\n"
        "description = LB/RB, LT/RT, Menu and View in the game's button font and the button guide, "
        "drawn by tools/xbox_prompts.py.\n"
        "version = 4\n", encoding="utf-8")
    if not a.no_enable:
        enable_in_mods_ini(user)
        print(f"enabled {MOD_NAME} in {user / 'mods.ini'}")
    print(f"layout: {layout}")


if __name__ == "__main__":
    main()
