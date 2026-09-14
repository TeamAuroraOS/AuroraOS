"""Minimal PNG reader: 8-bit RGB/RGBA/gray/gray+alpha/palette, no interlace."""
import struct, zlib

def read(path):
    b = open(path, 'rb').read()
    assert b[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    pos = 8
    idat = bytearray()
    plte = None
    trns = None
    w = h = bd = ct = None
    while pos < len(b):
        ln, typ = struct.unpack('>I4s', b[pos:pos + 8])
        data = b[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b'IHDR':
            w, h, bd, ct, _, _, inter = struct.unpack('>IIBBBBB', data)
            assert inter == 0, 'interlaced not supported'
        elif typ == b'PLTE':
            plte = data
        elif typ == b'tRNS':
            trns = data
        elif typ == b'IDAT':
            idat += data
        elif typ == b'IEND':
            break
    assert bd == 8, 'only 8-bit supported, got %d' % bd
    nch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    raw = zlib.decompress(bytes(idat))
    stride = w * nch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if f == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                c = prev[i - nch] if i >= nch else 0
                bb = prev[i]
                pa, pb, pc = abs(bb - c), abs(a - c), abs(a + bb - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (bb if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    # normalise to RGBA
    px = bytearray(w * h * 4)
    for i in range(w * h):
        s = out[i * nch:(i + 1) * nch]
        if ct == 6:   r, g, bl, a = s
        elif ct == 2: r, g, bl = s; a = 255
        elif ct == 0: r = g = bl = s[0]; a = 255
        elif ct == 4: r = g = bl = s[0]; a = s[1]
        else:
            idx = s[0]
            r, g, bl = plte[idx * 3:idx * 3 + 3]
            a = trns[idx] if trns and idx < len(trns) else 255
        px[i * 4:i * 4 + 4] = bytes((r, g, bl, a))
    return w, h, px
