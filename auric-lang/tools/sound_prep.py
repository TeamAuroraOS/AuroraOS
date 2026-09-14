"""Prepare WAV files for an Auric app's sound folder on the SD card.

Converts each source to mono 16-bit at --rate (22050 Hz by default),
area-filtered when reducing the rate, under an 8.3 name.

    python tools/sound_prep.py -o OUTDIR "long source name.wav=MOVE.WAV" ...

Sources may be 8, 16 or 24-bit PCM, mono or stereo. --gain is in decibels,
clipped at full scale.
"""
import argparse
import math
import os
import re
import struct
import sys
from array import array


def read_wav(path):
    """Return (rate, mono samples as floats in [-1, 1])."""
    data = open(path, 'rb').read()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise ValueError('not a RIFF/WAVE file')
    pos, fmt, body = 12, None, None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack('<I', data[pos + 4:pos + 8])[0]
        chunk = data[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', chunk[:16])
        elif cid == b'data':
            body = chunk
        pos += 8 + size + (size & 1)
    if fmt is None or body is None:
        raise ValueError('missing fmt or data chunk')
    tag, chans, rate, _, align, bits = fmt
    if tag not in (1, 0xFFFE) or bits not in (8, 16, 24) or chans not in (1, 2):
        raise ValueError('only 8/16/24-bit PCM, mono or stereo')

    frames = len(body) // align
    width = bits // 8
    if bits == 16:
        raw = array('h')
        raw.frombytes(body[:frames * align])
        if sys.byteorder != 'little':
            raw.byteswap()
        scale = 32768.0
        get = raw.__getitem__
    elif bits == 8:
        raw = body
        scale = 128.0
        get = lambda i: raw[i] - 128
    else:
        scale = 8388608.0
        get = lambda i: int.from_bytes(body[i * 3:i * 3 + 3], 'little', signed=True)

    out = array('d', bytes(8 * frames))
    for f in range(frames):
        v = get(f * chans)
        if chans == 2:
            v = (v + get(f * chans + 1)) / 2.0
        out[f] = v / scale
    return rate, out


def resample(src, src_rate, dst_rate):
    """Area filter going down (each output averages the input span it covers),
    linear interpolation going up."""
    if src_rate == dst_rate:
        return src
    n_out = int(len(src) * dst_rate // src_rate)
    out = array('d', bytes(8 * n_out))
    ratio = src_rate / dst_rate
    if ratio < 1.0:
        for j in range(n_out):
            x = j * ratio
            i = int(x)
            t = x - i
            b = src[i + 1] if i + 1 < len(src) else src[i]
            out[j] = src[i] * (1.0 - t) + b * t
        return out
    last = len(src)
    for j in range(n_out):
        a, b = j * ratio, (j + 1) * ratio
        ia, ib = int(a), min(int(math.ceil(b)), last)
        acc = 0.0
        for i in range(ia, ib):
            lo, hi = max(a, i), min(b, i + 1)
            acc += src[i] * (hi - lo)
        out[j] = acc / (b - a)
    return out


def write_wav(path, rate, samples, gain):
    pcm = array('h', bytes(2 * len(samples)))
    peak = 0.0
    for i, v in enumerate(samples):
        v *= gain
        peak = max(peak, abs(v))
        s = int(round(v * 32767.0))
        pcm[i] = 32767 if s > 32767 else (-32768 if s < -32768 else s)
    if sys.byteorder != 'little':
        pcm.byteswap()
    body = pcm.tobytes()
    head = (b'RIFF' + struct.pack('<I', 36 + len(body)) + b'WAVE' +
            b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, rate, rate * 2, 2, 16) +
            b'data' + struct.pack('<I', len(body)))
    with open(path, 'wb') as f:
        f.write(head + body)
    return len(head) + len(body), peak


NAME_83 = re.compile(r'^[A-Z0-9_\-]{1,8}\.[A-Z0-9]{1,3}$')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('pairs', nargs='+', help='SOURCE=NAME, NAME being 8.3')
    ap.add_argument('-o', '--out', required=True, help='output folder')
    ap.add_argument('--rate', type=int, default=22050)
    ap.add_argument('--gain', type=float, default=0.0, help='decibels')
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    gain = 10.0 ** (a.gain / 20.0)
    for pair in a.pairs:
        if '=' not in pair:
            ap.error('expected SOURCE=NAME, got %r' % pair)
        src, name = pair.rsplit('=', 1)
        name = name.upper()
        if not NAME_83.match(name):
            ap.error('%s is not an 8.3 name' % name)
        rate, mono = read_wav(src)
        out = resample(mono, rate, min(a.rate, rate))
        size, peak = write_wav(os.path.join(a.out, name), min(a.rate, rate), out,
                               gain)
        db = 20.0 * math.log10(peak) if peak > 0 else float('-inf')
        print('%-12s %6.2f s  %5d Hz  %8d bytes  peak %6.1f dBFS%s' %
              (name, len(out) / float(min(a.rate, rate)), min(a.rate, rate), size,
               db, '  (clipped)' if peak > 1.0 else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
