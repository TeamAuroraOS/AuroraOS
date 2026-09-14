"""Minimal TrueType reader and anti-aliased rasteriser: cmap format 4,
glyf/loca (simple and composite) and hmtx. Outlines are flattened and filled
with the nonzero winding rule, with coverage from sub-scanline span overlap.
"""

import struct

SUBSAMPLES = 5          # sub-scanlines per pixel row
CURVE_STEPS = 8         # line segments per quadratic


class Font(object):
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        if d[:4] not in (b'\x00\x01\x00\x00', b'true'):
            raise ValueError('not a TrueType file: %s' % path)
        num = struct.unpack('>H', d[4:6])[0]
        self.tables = {}
        for i in range(num):
            o = 12 + i * 16
            tag = d[o:o + 4].decode('latin1')
            off, ln = struct.unpack('>II', d[o + 8:o + 16])
            self.tables[tag] = (off, ln)

        head = self.tables['head'][0]
        self.units_per_em = struct.unpack('>H', d[head + 18:head + 20])[0]
        self.index_to_loc = struct.unpack('>h', d[head + 50:head + 52])[0]

        maxp = self.tables['maxp'][0]
        self.num_glyphs = struct.unpack('>H', d[maxp + 4:maxp + 6])[0]

        hhea = self.tables['hhea'][0]
        self.ascent = struct.unpack('>h', d[hhea + 4:hhea + 6])[0]
        self.descent = struct.unpack('>h', d[hhea + 6:hhea + 8])[0]
        self.line_gap = struct.unpack('>h', d[hhea + 8:hhea + 10])[0]
        self.num_hmetrics = struct.unpack('>H', d[hhea + 34:hhea + 36])[0]

        self._load_loca()
        self._load_cmap()

    def _load_loca(self):
        off, ln = self.tables['loca']
        d = self.data
        n = self.num_glyphs + 1
        if self.index_to_loc == 0:
            raw = struct.unpack('>%dH' % n, d[off:off + n * 2])
            self.loca = [v * 2 for v in raw]
        else:
            self.loca = list(struct.unpack('>%dI' % n, d[off:off + n * 4]))

    def _load_cmap(self):
        off = self.tables['cmap'][0]
        d = self.data
        n = struct.unpack('>H', d[off + 2:off + 4])[0]
        best = None
        for i in range(n):
            o = off + 4 + i * 8
            pid, eid, sub = struct.unpack('>HHI', d[o:o + 8])
            fmt = struct.unpack('>H', d[off + sub:off + sub + 2])[0]
            if fmt == 4 and (pid == 3 and eid in (1, 10) or pid == 0):
                best = off + sub
                break
        if best is None:
            raise ValueError('no usable cmap subtable')

        seg2 = struct.unpack('>H', d[best + 6:best + 8])[0]
        seg = seg2 // 2
        p = best + 14
        self.cmap_end = struct.unpack('>%dH' % seg, d[p:p + seg2]); p += seg2 + 2
        self.cmap_start = struct.unpack('>%dH' % seg, d[p:p + seg2]); p += seg2
        self.cmap_delta = struct.unpack('>%dh' % seg, d[p:p + seg2]); p += seg2
        self.cmap_range_off_pos = p
        self.cmap_range_off = struct.unpack('>%dH' % seg, d[p:p + seg2])
        self.cmap_segs = seg

    def glyph_index(self, ch):
        c = ord(ch)
        for i in range(self.cmap_segs):
            if self.cmap_end[i] >= c:
                if self.cmap_start[i] > c:
                    return 0
                ro = self.cmap_range_off[i]
                if ro == 0:
                    return (c + self.cmap_delta[i]) & 0xFFFF
                pos = self.cmap_range_off_pos + i * 2 + ro + (c - self.cmap_start[i]) * 2
                g = struct.unpack('>H', self.data[pos:pos + 2])[0]
                return (g + self.cmap_delta[i]) & 0xFFFF if g else 0
        return 0

    def advance(self, gid):
        off = self.tables['hmtx'][0]
        if gid >= self.num_hmetrics:
            gid = self.num_hmetrics - 1
        return struct.unpack('>H', self.data[off + gid * 4:off + gid * 4 + 2])[0]

    def contours(self, gid, depth=0):
        """Glyph outline as a list of contours, each a list of (x, y, on_curve)."""
        if gid >= self.num_glyphs or self.loca[gid] == self.loca[gid + 1]:
            return []
        base = self.tables['glyf'][0] + self.loca[gid]
        d = self.data
        ncont = struct.unpack('>h', d[base:base + 2])[0]
        if ncont < 0:
            return self._composite(base + 10, depth)

        p = base + 10
        ends = struct.unpack('>%dH' % ncont, d[p:p + ncont * 2]); p += ncont * 2
        npts = ends[-1] + 1 if ncont else 0
        ilen = struct.unpack('>H', d[p:p + 2])[0]; p += 2 + ilen

        flags = []
        while len(flags) < npts:
            f = d[p]; p += 1
            flags.append(f)
            if f & 8:
                r = d[p]; p += 1
                flags.extend([f] * r)
        flags = flags[:npts]

        xs, v = [], 0
        for f in flags:
            if f & 2:
                dx = d[p]; p += 1
                v += dx if f & 16 else -dx
            elif not (f & 16):
                v += struct.unpack('>h', d[p:p + 2])[0]; p += 2
            xs.append(v)
        ys, v = [], 0
        for f in flags:
            if f & 4:
                dy = d[p]; p += 1
                v += dy if f & 32 else -dy
            elif not (f & 32):
                v += struct.unpack('>h', d[p:p + 2])[0]; p += 2
            ys.append(v)

        out, s = [], 0
        for e in ends:
            out.append([(xs[i], ys[i], bool(flags[i] & 1)) for i in range(s, e + 1)])
            s = e + 1
        return out

    def _composite(self, p, depth):
        if depth > 4:
            return []
        d, out = self.data, []
        while True:
            flags, gi = struct.unpack('>HH', d[p:p + 4]); p += 4
            if flags & 1:
                a1, a2 = struct.unpack('>hh', d[p:p + 4]); p += 4
            else:
                a1, a2 = struct.unpack('>bb', d[p:p + 2]); p += 2
            sx = sy = 1.0
            s01 = s10 = 0.0
            if flags & 8:
                sx = sy = _f2dot14(d, p); p += 2
            elif flags & 0x40:
                sx = _f2dot14(d, p); sy = _f2dot14(d, p + 2); p += 4
            elif flags & 0x80:
                sx = _f2dot14(d, p); s01 = _f2dot14(d, p + 2)
                s10 = _f2dot14(d, p + 4); sy = _f2dot14(d, p + 6); p += 8
            dx, dy = (a1, a2) if flags & 2 else (0, 0)
            for c in self.contours(gi, depth + 1):
                out.append([(x * sx + y * s10 + dx, x * s01 + y * sy + dy, on)
                            for (x, y, on) in c])
            if not (flags & 0x20):
                break
        return out

    def render(self, ch, px_size):
        """Render one character. Returns (bitmap, w, h, left, top, advance).

        bitmap is w*h bytes of coverage; left/top place it relative to the pen
        position on the baseline, with top counted upward.
        """
        gid = self.glyph_index(ch)
        scale = float(px_size) / self.units_per_em
        adv = int(round(self.advance(gid) * scale))
        conts = self.contours(gid)

        segs = []
        for c in conts:
            segs.extend(_flatten(c, scale))
        if not segs:
            return bytearray(), 0, 0, 0, 0, adv

        xs = [p for s in segs for p in (s[0], s[2])]
        ys = [p for s in segs for p in (s[1], s[3])]
        x0, x1 = _floor(min(xs)), _ceil(max(xs))
        y0, y1 = _floor(min(ys)), _ceil(max(ys))
        w, h = x1 - x0, y1 - y0
        if w <= 0 or h <= 0:
            return bytearray(), 0, 0, 0, 0, adv

        # y grows upward in font units; the bitmap runs top-down.
        cov = [0.0] * (w * h)
        for row in range(h):
            row_top = y1 - row            # font-space y at the top of this row
            acc = cov[row * w:(row + 1) * w]
            for s in range(SUBSAMPLES):
                sy = row_top - (s + 0.5) / SUBSAMPLES
                xs_cross = []
                for (ax, ay, bx, by) in segs:
                    if ay == by:
                        continue
                    lo, hi = (ay, by) if ay < by else (by, ay)
                    if not (lo <= sy < hi):
                        continue
                    t = (sy - ay) / (by - ay)
                    xs_cross.append((ax + t * (bx - ax), 1 if by > ay else -1))
                if not xs_cross:
                    continue
                xs_cross.sort()
                wind = 0
                for i in range(len(xs_cross) - 1):
                    wind += xs_cross[i][1]
                    if wind != 0:
                        _span(acc, xs_cross[i][0] - x0, xs_cross[i + 1][0] - x0,
                              w, 1.0 / SUBSAMPLES)
            cov[row * w:(row + 1) * w] = acc

        bmp = bytearray(w * h)
        for i, v in enumerate(cov):
            n = int(v * 255.0 + 0.5)
            bmp[i] = 0 if n < 0 else (255 if n > 255 else n)
        return bmp, w, h, x0, y1, adv


def _f2dot14(d, p):
    return struct.unpack('>h', d[p:p + 2])[0] / 16384.0


def _floor(v):
    return int(v) if v == int(v) or v > 0 else int(v) - 1


def _ceil(v):
    return int(v) if v == int(v) else (int(v) + 1 if v > 0 else int(v))


def _span(acc, xa, xb, w, weight):
    """Add `weight` coverage over [xa, xb) in pixel units, with fractional ends."""
    if xb <= xa:
        return
    if xa < 0:
        xa = 0.0
    if xb > w:
        xb = float(w)
    if xb <= xa:
        return
    ia, ib = int(xa), int(xb)
    if ia == ib:
        acc[ia] += (xb - xa) * weight
        return
    acc[ia] += (ia + 1 - xa) * weight
    for i in range(ia + 1, ib):
        acc[i] += weight
    if ib < w:
        acc[ib] += (xb - ib) * weight


def _flatten(contour, scale):
    """One contour of (x, y, on_curve) points to scaled line segments."""
    pts = [(x * scale, y * scale, on) for (x, y, on) in contour]
    if not pts:
        return []

    # TrueType allows an implied on-curve point midway between two off-curve
    # points, and a contour may start off-curve.
    exp = []
    n = len(pts)
    for i in range(n):
        x, y, on = pts[i]
        if not on:
            px, py, pon = pts[i - 1]
            if not pon:
                exp.append(((px + x) / 2.0, (py + y) / 2.0, True))
        exp.append((x, y, on))
    if not exp[0][2]:
        for i, p in enumerate(exp):
            if p[2]:
                exp = exp[i:] + exp[:i]
                break
        else:
            return []

    segs = []
    i, n = 0, len(exp)
    cx, cy = exp[0][0], exp[0][1]
    start = (cx, cy)
    i = 1
    while i <= n:
        px, py, on = exp[i % n]
        if on:
            segs.append((cx, cy, px, py))
            cx, cy = px, py
            i += 1
        else:
            nx, ny, _ = exp[(i + 1) % n]
            for s in range(1, CURVE_STEPS + 1):
                t = float(s) / CURVE_STEPS
                u = 1.0 - t
                qx = u * u * cx + 2 * u * t * px + t * t * nx
                qy = u * u * cy + 2 * u * t * py + t * t * ny
                segs.append((cx, cy, qx, qy) if s == 1 else (segs[-1][2], segs[-1][3], qx, qy))
            cx, cy = nx, ny
            i += 2
    if (cx, cy) != start:
        segs.append((cx, cy, start[0], start[1]))
    return segs
