"""DSP-ADPCM, the 4-bit format of the GameCube and 3DS DSPs, for assets.pak.

A stream is 8 predictor pairs (16 Q11 coefficients) and 8-byte frames: a
header byte (pair << 4 | shift), then 14 signed 4-bit residuals, high nibble
first. Each sample is the pair's prediction from the two decoded samples
before it plus residual << shift. decode() is the arithmetic of src/assets.c.

Standard library only, so the Makefile's Python can run it.
"""

import math
from operator import mul

FRAME = 14       # samples per 8-byte frame
PAIRS = 8
COEF_MAX = 4075  # |c| < 2 in Q11, which keeps the decoder's sums in 32 bits
MAX_SHIFT = 12


def resample(x, src_rate, dst_rate, half=32, phases=256):
    """`x` (a list of numbers) at `dst_rate`: a Blackman-windowed sinc of
    2 * `half` taps, cut off at 90% of the lower Nyquist rate."""
    ratio = src_rate / dst_rate
    fc = 0.45 * min(1.0, 1.0 / ratio)  # cycles per source sample
    taps = []
    for p in range(phases):
        frac = p / phases
        row = []
        for m in range(-half + 1, half + 1):
            t = m - frac
            arg = 2.0 * fc * t
            s = 2.0 * fc * (math.sin(math.pi * arg) / (math.pi * arg) if arg else 1.0)
            w = (0.42 + 0.5 * math.cos(math.pi * t / half) +
                 0.08 * math.cos(2.0 * math.pi * t / half))
            row.append(s * w)
        g = sum(row)
        taps.append([v / g for v in row])

    xp = [0.0] * half + list(x) + [0.0] * (half + 1)
    n_out = int(len(x) * dst_rate / src_rate)
    out = []
    for k in range(n_out):
        t = k * ratio
        i = int(t)
        p = int(round((t - i) * phases))
        if p == phases:
            i += 1
            p = 0
        out.append(sum(map(mul, taps[p], xp[i + 1:i + 2 * half + 1])))
    return out


def to_s16(x):
    """Rounded and clipped to 16 bits; also the number of samples clipped."""
    out, clipped = [], 0
    for v in x:
        v = int(round(v))
        if v > 32767:
            v, clipped = 32767, clipped + 1
        elif v < -32768:
            v, clipped = -32768, clipped + 1
        out.append(v)
    return out, clipped


def design(x):
    """Eight predictor pairs for `x`: (0, 0), and seven k-means centres of the
    least-squares predictors of its frames."""
    pts = []
    for i in range(0, len(x), FRAME):
        r11 = r12 = r22 = r1 = r2 = 0
        for j in range(i, min(i + FRAME, len(x))):
            a = x[j - 1] if j >= 1 else 0
            b = x[j - 2] if j >= 2 else 0
            s = x[j]
            r11 += a * a
            r12 += a * b
            r22 += b * b
            r1 += s * a
            r2 += s * b
        det = r11 * r22 - r12 * r12
        if det <= 0:
            continue
        c1 = max(-1.99, min(1.99, (r1 * r22 - r2 * r12) / det))
        c2 = max(-1.99, min(1.99, (r2 * r11 - r1 * r12) / det))
        pts.append((c1, c2))

    centres = [(0.0, 0.0)]
    if pts:
        pts.sort()
        step = len(pts) / (PAIRS - 1)
        for k in range(PAIRS - 1):
            grp = pts[int(k * step):max(int(k * step) + 1, int((k + 1) * step))]
            centres.append((sum(p[0] for p in grp) / len(grp),
                            sum(p[1] for p in grp) / len(grp)))
        for _ in range(20):
            sums = [[0.0, 0.0, 0] for _ in centres]
            for a, b in pts:
                best, bk = None, 0
                for k, (ca, cb) in enumerate(centres):
                    d = (a - ca) * (a - ca) + (b - cb) * (b - cb)
                    if best is None or d < best:
                        best, bk = d, k
                acc = sums[bk]
                acc[0] += a
                acc[1] += b
                acc[2] += 1
            moved = [(s[0] / s[2], s[1] / s[2]) if s[2] else c
                     for s, c in zip(sums, centres)]
            moved[0] = (0.0, 0.0)
            if moved == centres:
                break
            centres = moved
    while len(centres) < PAIRS:
        centres.append((0.0, 0.0))
    coefs = []
    for a, b in centres:
        coefs.append(max(-COEF_MAX, min(COEF_MAX, int(round(a * 2048)))))
        coefs.append(max(-COEF_MAX, min(COEF_MAX, int(round(b * 2048)))))
    return coefs


def _trial(seg, c1, c2, sh, h1, h2, limit):
    half = (1 << sh) >> 1
    err = 0
    codes = []
    for s in seg:
        p = (c1 * h1 + c2 * h2 + 1024) >> 11
        n = (s - p + half) >> sh
        if n > 7:
            n = 7
        elif n < -8:
            n = -8
        y = p + (n << sh)
        if y > 32767:
            y = 32767
        elif y < -32768:
            y = -32768
        err += (s - y) * (s - y)
        if err >= limit:
            return None
        codes.append(n)
        h2, h1 = h1, y
    return err, codes, h1, h2


def encode(x, coefs):
    """The frames of `x` (16-bit integers) as bytes, choosing for each frame the
    pair and shift with the least error. Decoding starts from silence."""
    pairs = [(coefs[2 * k], coefs[2 * k + 1]) for k in range(PAIRS)]
    out = bytearray()
    h1 = h2 = 0
    for i in range(0, len(x), FRAME):
        seg = x[i:i + FRAME]
        seg = seg + [0] * (FRAME - len(seg))
        best = None
        for k, (c1, c2) in enumerate(pairs):
            peak, a1, a2 = 0, h1, h2
            for s in seg:
                r = abs(s - ((c1 * a1 + c2 * a2 + 1024) >> 11))
                if r > peak:
                    peak = r
                a2, a1 = a1, s
            est = 0
            while est < MAX_SHIFT and peak > (15 << est) >> 1:
                est += 1
            for sh in (est, est + 1, est - 1, est + 2):
                if not 0 <= sh <= MAX_SHIFT:
                    continue
                t = _trial(seg, c1, c2, sh, h1, h2,
                           best[0] if best else 1 << 62)
                if t:
                    best = (t[0], k, sh, t[1], t[2], t[3])
        _, k, sh, codes, h1, h2 = best
        out.append((k << 4) | sh)
        for j in range(0, FRAME, 2):
            out.append(((codes[j] & 15) << 4) | (codes[j + 1] & 15))
    return bytes(out)


def decode(coefs, data, samples):
    """The samples of a stream, as src/assets.c decodes them."""
    out = []
    h1 = h2 = 0
    for f in range(0, len(data), 8):
        k, sh = data[f] >> 4, data[f] & 15
        c1, c2 = coefs[2 * k], coefs[2 * k + 1]
        for j in range(FRAME):
            if len(out) == samples:
                return out
            b = data[f + 1 + j // 2]
            n = (b & 15) if j & 1 else (b >> 4)
            if n >= 8:
                n -= 16
            y = (n * (2048 << sh) + 1024 + c1 * h1 + c2 * h2) >> 11
            y = max(-32768, min(32767, y))
            out.append(y)
            h2, h1 = h1, y
    return out
