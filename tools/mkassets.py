"""Build SD:\\Aurora\\assets.pak from the PNG art, the Figtree fonts and the
OS's sounds.

Every size the UI draws is resampled here once, with an area filter, so
nothing is scaled at runtime. The white-on-alpha icons are stored as
coverage and tinted when drawn. Payloads are byte-RLE'd. Sounds are mono at
the CSND mixer's rate, as DSP-ADPCM, in a section after the images; see
docs/assets.md.

Usage:
    python tools/mkassets.py [-o Aurora/assets.pak] [--header include/asset_ids.h]
"""

import ttf
import png_read
import dspadpcm
import argparse
import array
import math
import os
import struct
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PAK_VERSION = 1
T_ALPHA8, T_RGBA, T_FONT = 0, 1, 2

# 24 is the File Explorer's list rows and 16 the status bar; both are drawn
# from the same art, never scaled at runtime.
ICON_SIZES = (64, 48, 32, 24, 16)

# (asset id, source file). Every one of these is a white-on-alpha mask, drawn
# tinted, so one file serves every accent colour.
MASKS = [
    ('CHECK',        'Check.png'),
    ('SETTINGS',     'Cog.png'),
    ('CROSS',        'Cross.png'),
    ('GLOBE',        'Globe.png'),
    ('APPS',         'Grid 2x2.png'),
    ('INFO',         'Info Circle.png'),
    ('LOCK',         'Lock.png'),
    ('CPU',          'MHz Mode.png'),
    ('MUSIC',        'Music-Icon.png'),
    ('WIFI_OFF',     'No Connection.png'),
    ('DISPLAY',      'O3DS (fill).png'),
    ('CONSOLE',      'O3DS.png'),
    ('HELP',         'Question Mark Circle.png'),
    ('REPORT',       'Report Issue.png'),
    ('STREETPASS',   'StreetPass.png'),
    ('MONITOR',      'System Monitor.png'),
    ('POWER',        'Power.png'),
    ('TOGGLE',       'Toggle.png'),
    ('UNKNOWN',      'Unkonwn File.png'),
    ('USER',         'User Profile.png'),
    ('VOLUME_HIGH',  'Volume High.png'),
    ('VOLUME_LOW',   'Volume Low.png'),
    ('VOLUME_MUTE',  'Volume Muting.png'),
    ('WIFI',         'WiFi-good.png'),
    ('FILE_MEDIA',   'Media-File-Icon.png'),
    ('FILE_AURORA',  'Aurora-Bin-File.png'),
    ('FILE_BIN',     'Unknown-Bin-File.png'),
    ('CUBE',         'Cube.png'),
    ('FOLDER',       'folder-icon-without-text.png'),
    ('SEARCH',       'Search.png'),
    ('WRENCH',       'Wrench.png'),
]

# Full-colour art, kept as RGBA and drawn untinted: game card.png has grey
# shading that coverage would flatten.
#
# (asset id, source, size, rounded). `rounded` cuts a square tile's corners to a
# radius of ROUND_FRAC of its size.
ROUND_FRAC = 0.22
COLOR = [
    ('APP_FILES',  'files.png',       64, True),
    ('APP_FILES',  'files.png',       32, True),
    ('APP_FILES',  'files.png',       48, True),
    ('APP_FILES',  'files.png',       24, True),
    ('APP_FILES',  'files.png',       16, True),
    ('APP_PREFS',  'preferences.png', 64, True),
    ('APP_STORE',  'store.png',       64, True),
    ('APP_STORE',  'store.png',       32, True),
    ('APP_STORE',  'store.png',       16, True),
    ('ICON_GAMECARD', 'game card.png', 64, False),
    ('ICON_GAMECARD', 'game card.png', 32, False),
    ('ICON_GAMECARD', 'game card.png', 16, False),
]

# Full-colour art that is not square, kept at its own size: the auroraShop
# logo the Home Menu shows for the store. (asset id, source, width, height).
ART = [
    ('STORE_LOGO', 'store-logo.png', 191, 152),
]

# Faces for the About page's More Info credits, cut to a circle rather than to
# ROUND_FRAC. (asset id, source, size).
PEOPLE = [
    ('PERSON_DISLOPIK',  'people/dislopik.png',  40),
    ('PERSON_STAR',      'people/star.png',      40),
    ('PERSON_ATEXBG',    'people/atexbg.png',    40),
    ('PERSON_SAIITANAA', 'people/saiitanaa.png', 40),
    ('PERSON_KYAERO',    'people/kyaero.png',    40),
    ('PERSON_SECRET',    'people/secret.png',    40),
    ('PERSON_LEWECK',    'people/leweck.png',    40),
]

# (asset id, ttf, pixel size). Each atlas covers U+0020..U+00FF (Latin-1). The
# C1 controls and anything the font lacks become empty entries rather than the
# missing-glyph box.
CHARS = ''.join(chr(c) for c in range(0x20, 0x100))
FONTS = [
    ('FONT_SMALL', 'Figtree-Regular.ttf', 11),
    ('FONT_UI',    'Figtree-Regular.ttf', 14),
    ('FONT_BOLD',  'Figtree-Bold.ttf',    14),
    ('FONT_TITLE', 'Figtree-Bold.ttf',    20),
]

WALLPAPER = ('WALLPAPER', 'background.png', 400, 240)

# (sound id, source WAV, which digital silence to drop). The two songs loop, and
# their 17 ms of leading silence would put every pass off the beat. The menu
# sounds come as 3.2 s files that are mostly silence.
TRIM_NONE, TRIM_LEAD, TRIM_BOTH = 0, 1, 2
SOUNDS = [
    ('STARTUP', 'startup.wav',     TRIM_NONE),
    ('SHOP',    'Aurora-SHOP.wav', TRIM_LEAD),
    ('DL_DONE', 'DL-complete.wav', TRIM_LEAD),
    ('SELECT',  'select.wav',      TRIM_BOTH),
    ('BACK',    'back.wav',        TRIM_BOTH),
    ('MOVE',    'move.wav',        TRIM_BOTH),
    ('HOME',    'Aurora-HOME.wav', TRIM_LEAD),
]
SND_DSPADPCM = 0

# The core plays a sound with the divider CSND_CLOCK // rate, so each one is
# resampled to exactly the rate that divider gives: 32728.5 Hz, CSND's mixer
# rate, at which the channel copies samples through unchanged.
CSND_CLOCK = 67027964
CSND_RATE = 32728


def read_sound(path, trim):
    """A WAV file as 16-bit mono at CSND_RATE, and a line describing it."""
    with wave.open(path, 'rb') as w:
        ch, width = w.getnchannels(), w.getsampwidth()
        rate, n = w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 2:
        s = array.array('h', raw)
        if sys.byteorder == 'big':
            s.byteswap()
    elif width == 1:
        s = array.array('h', ((b - 128) << 8 for b in raw))
    else:
        raise SystemExit('%s: only 8- and 16-bit PCM' % path)
    lead, end = 0, n
    if trim != TRIM_NONE:
        while lead < n and not any(s[lead * ch:(lead + 1) * ch]):
            lead += 1
    if trim == TRIM_BOTH:
        while end > lead and not any(s[(end - 1) * ch:end * ch]):
            end -= 1
    mono = [sum(s[i:i + ch]) / ch for i in range(lead * ch, end * ch, ch)]
    if not mono:
        raise SystemExit('%s: no samples' % path)
    out = CSND_CLOCK / (CSND_CLOCK // CSND_RATE)
    x, clipped = dspadpcm.to_s16(dspadpcm.resample(mono, rate, out))
    note = '%d Hz x%d, %.3f s' % (rate, ch, n / rate)
    if lead:
        note += ', %.1f ms of silence cut' % (1000.0 * lead / rate)
    if end < n:
        note += ', %.3f s cut at the end' % ((n - end) / rate)
    if clipped:
        note += ', %d samples clipped' % clipped
    return x, note


def area_alpha(w, h, px, nw, nh):
    """Box-filter the alpha channel to nw x nh."""
    out = bytearray(nw * nh)
    xr = [(dx * w // nw, max(dx * w // nw + 1, (dx + 1) * w // nw))
          for dx in range(nw)]
    for dy in range(nh):
        y0, y1 = dy * h // nh, max(dy * h // nh + 1, (dy + 1) * h // nh)
        row = dy * nw
        for dx in range(nw):
            x0, x1 = xr[dx]
            tot = n = 0
            for sy in range(y0, y1):
                b = sy * w
                for sx in range(x0, x1):
                    tot += px[(b + sx) * 4 + 3]
                    n += 1
            out[row + dx] = tot // n
    return out


def area_rgba(w, h, px, nw, nh):
    """Box-filter all four channels, averaging colour weighted by alpha so
    transparent pixels do not drag the edges toward black."""
    out = bytearray(nw * nh * 4)
    xr = [(dx * w // nw, max(dx * w // nw + 1, (dx + 1) * w // nw))
          for dx in range(nw)]
    for dy in range(nh):
        y0, y1 = dy * h // nh, max(dy * h // nh + 1, (dy + 1) * h // nh)
        for dx in range(nw):
            x0, x1 = xr[dx]
            ar = ag = ab = aa = n = 0
            for sy in range(y0, y1):
                b = sy * w
                for sx in range(x0, x1):
                    o = (b + sx) * 4
                    a = px[o + 3]
                    ar += px[o] * a
                    ag += px[o + 1] * a
                    ab += px[o + 2] * a
                    aa += a
                    n += 1
            o = (dy * nw + dx) * 4
            if aa:
                out[o] = ar // aa
                out[o + 1] = ag // aa
                out[o + 2] = ab // aa
            out[o + 3] = aa // n
    return out


def round_corners(px, sz, frac):
    """Scale the alpha of an sz x sz RGBA image by the coverage of a rounded
    square of radius frac*sz, sampled 4x4 per pixel so the curve is smooth."""
    r = max(1.0, sz * frac)
    out = bytearray(px)
    for y in range(sz):
        for x in range(sz):
            hit = 0
            for sy in range(4):
                for sx in range(4):
                    fx, fy = x + (sx + 0.5) / 4.0, y + (sy + 0.5) / 4.0
                    cx, cy = min(max(fx, r), sz - r), min(max(fy, r), sz - r)
                    if (fx - cx) ** 2 + (fy - cy) ** 2 <= r * r:
                        hit += 1
            if hit < 16:
                o = (y * sz + x) * 4 + 3
                out[o] = out[o] * hit // 16
    return out


def text_page(w, h, px):
    """The text-file icon: the unknown-file page with its question mark cleared
    and ruled lines drawn in. Coordinates are in the 96x112 source art."""
    assert (w, h) == (96, 112), 'Unkonwn File.png changed size'
    out = bytearray(px)

    def fill(x0, y0, x1, y1, a):
        for y in range(y0, y1):
            for x in range(x0, x1):
                o = (y * w + x) * 4
                out[o] = out[o + 1] = out[o + 2] = 255
                out[o + 3] = a

    fill(8, 52, 88, 104, 0)  # the question mark
    for x0, y0, x1 in ((20, 24, 40), (20, 56, 76), (20, 74, 76), (20, 92, 60)):
        fill(x0, y0, x1, y0 + 8, 255)
    return out


def rle(data):
    """Byte RLE. 0x01..0x7F = run of n of the next byte; 0x81..0xFF = n
    literals follow. Decoder is a dozen lines; see src/assets.c."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        run = 1
        while i + run < n and data[i + run] == data[i] and run < 127:
            run += 1
        if run >= 3:
            out += bytes((run, data[i]))
            i += run
            continue
        lit = bytearray()
        while i < n and len(lit) < 127:
            r = 1
            while i + r < n and data[i + r] == data[i] and r < 4:
                r += 1
            if r >= 3:
                break
            lit.append(data[i])
            i += 1
        out += bytes((0x80 | len(lit),)) + lit
    return out


def unrle(data, usize):
    out = bytearray()
    i = 0
    while i < len(data) and len(out) < usize:
        c = data[i]
        i += 1
        if c & 0x80:
            k = c & 0x7F
            out += data[i:i + k]
            i += k
        else:
            out += bytes((data[i],)) * c
            i += 1
    return out


def build_font(path, size, chars, atlas_w=256):
    f = ttf.Font(path)
    scale = float(size) / f.units_per_em
    glyphs = []
    for ch in chars:
        cp = ord(ch)
        if 0x7F <= cp <= 0x9F or f.glyph_index(ch) == 0:
            glyphs.append((ch, bytearray(), 0, 0, 0, 0, 0))
        else:
            glyphs.append((ch,) + f.render(ch, size))

    x = y = row_h = 0
    placed = []
    for (ch, bmp, w, h, left, top, adv) in glyphs:
        if w and x + w > atlas_w:
            x, y, row_h = 0, y + row_h + 1, 0
        placed.append((ch, bmp, w, h, left, top, adv, x, y))
        if w:
            x += w + 1
            row_h = max(row_h, h)
    atlas_h = y + row_h + 1

    atlas = bytearray(atlas_w * atlas_h)
    for (ch, bmp, w, h, left, top, adv, ax, ay) in placed:
        for r in range(h):
            o = (ay + r) * atlas_w + ax
            atlas[o:o + w] = bmp[r * w:(r + 1) * w]

    asc = int(round(f.ascent * scale))
    desc = int(round(f.descent * scale))
    line = int(round((f.ascent - f.descent + f.line_gap) * scale))
    body = struct.pack('<HHhhhHHH', ord(chars[0]), len(chars), asc, desc, line,
                       atlas_w, atlas_h, 0)
    for (ch, bmp, w, h, left, top, adv, ax, ay) in placed:
        body += struct.pack('<bbBBBBHH', _clamp8(left), _clamp8(top), w, h,
                            adv, 0, ax, ay)
    return body + bytes(atlas), atlas_w, atlas_h


def _clamp8(v):
    return -128 if v < -128 else (127 if v > 127 else v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-o', '--out', default=os.path.join('Aurora', 'assets.pak'))
    ap.add_argument('--header', default=os.path.join('include', 'asset_ids.h'))
    ap.add_argument('--icons', default='icons')
    ap.add_argument('--fonts', default=os.path.join('assets', 'fonts'))
    ap.add_argument('--audio', default=os.path.join('assets', 'audio'))
    ap.add_argument('-v', '--verbose', action='store_true')
    a = ap.parse_args()

    entries = []   # (name, type, w, h, payload)

    for name, fn in MASKS:
        src = os.path.join(a.icons, fn)
        w, h, px = png_read.read(src)
        for sz in ICON_SIZES:
            # keep the aspect of non-square art, fitting the long side
            if w >= h:
                nw, nh = sz, max(1, h * sz // w)
            else:
                nw, nh = max(1, w * sz // h), sz
            entries.append(('ICON_%s_%d' % (name, sz), T_ALPHA8, nw, nh,
                            area_alpha(w, h, px, nw, nh)))
        if a.verbose:
            print('  mask  %-24s %dx%d' % (fn, w, h))

    # Derived mask: the text-file icon, built from the unknown-file page.
    w, h, px = png_read.read(os.path.join(a.icons, 'Unkonwn File.png'))
    px = text_page(w, h, px)
    for sz in ICON_SIZES:
        nw = max(1, w * sz // h)
        entries.append(('ICON_FILE_TEXT_%d' % sz, T_ALPHA8, nw, sz,
                        area_alpha(w, h, px, nw, sz)))

    for name, fn, sz, rounded in COLOR:
        w, h, px = png_read.read(os.path.join(a.icons, fn))
        rgba = area_rgba(w, h, px, sz, sz)
        if rounded:
            rgba = round_corners(rgba, sz, ROUND_FRAC)
        entries.append(('%s_%d' % (name, sz), T_RGBA, sz, sz, rgba))
        if a.verbose:
            print('  color %-24s %dx%d' % (fn, w, h))

    for name, fn, aw, ah in ART:
        w, h, px = png_read.read(os.path.join(a.icons, fn))
        entries.append((name, T_RGBA, aw, ah, area_rgba(w, h, px, aw, ah)))
        if a.verbose:
            print('  art   %-24s %dx%d -> %dx%d' % (fn, w, h, aw, ah))

    for name, fn, sz in PEOPLE:
        w, h, px = png_read.read(os.path.join(a.icons, fn))
        rgba = round_corners(area_rgba(w, h, px, sz, sz), sz, 0.5)
        entries.append(('%s_%d' % (name, sz), T_RGBA, sz, sz, rgba))
        if a.verbose:
            print('  face  %-24s %dx%d' % (fn, w, h))

    name, fn, ww, wh = WALLPAPER
    w, h, px = png_read.read(os.path.join(a.icons, fn))
    entries.append((name, T_ALPHA8, ww, wh, area_alpha(w, h, px, ww, wh)))
    if a.verbose:
        print('  paper %-24s %dx%d -> %dx%d' % (fn, w, h, ww, wh))

    for name, fn, sz in FONTS:
        body, aw, ah = build_font(os.path.join(a.fonts, fn), sz, CHARS)
        entries.append((name, T_FONT, aw, ah, body))
        if a.verbose:
            print('  font  %-24s %2dpx atlas %dx%d' % (fn, sz, aw, ah))

    ids = [e[0] for e in entries]
    hdr = struct.calcsize('<8I')
    esz = struct.calcsize('<IHHHHIII')
    data_off = hdr + esz * len(entries)

    blob = bytearray()
    table = b''
    arena = 0
    max_csize = 0
    for i, (name, typ, w, h, payload) in enumerate(entries):
        comp = rle(payload)
        assert unrle(comp, len(payload)) == payload, 'RLE round-trip failed: ' + name
        table += struct.pack('<IHHHHIII', i, typ, w, h, 0,
                             data_off + len(blob), len(comp), len(payload))
        blob += comp
        arena += (len(payload) + 3) & ~3
        max_csize = max(max_csize, len(comp))

    # Loaders from before the sound section check nothing past the images, so
    # they still take this pack.
    sound_off = data_off + len(blob)
    ssz = struct.calcsize('<6I')
    sound_table = b''
    sound_blob = bytearray()
    for i, (name, fn, trim) in enumerate(SOUNDS):
        x, note = read_sound(os.path.join(a.audio, fn), trim)
        coefs = dspadpcm.design(x)
        frames = dspadpcm.encode(x, coefs)
        dec = dspadpcm.decode(coefs, frames, len(x))
        assert len(dec) == len(x), 'DSP-ADPCM round-trip failed: ' + name
        payload = struct.pack('<16h', *coefs) + frames
        sound_table += struct.pack('<6I', i, SND_DSPADPCM, CSND_RATE, len(x),
                                   sound_off + ssz * len(SOUNDS) + len(sound_blob),
                                   len(payload))
        sound_blob += payload
        if a.verbose:
            err = sum((p - q) * (p - q) for p, q in zip(x, dec))
            snr = 10 * math.log10(sum(p * p for p in x) / err) if err else 99.0
            print('  sound %-24s %s -> %d samples, %d bytes, %.1f dB' %
                  (fn, note, len(x), len(payload), snr))

    out = struct.pack('<8I', 0x4B415041, PAK_VERSION, len(entries), data_off,
                      arena, sound_off, len(SOUNDS), 0) + table + bytes(blob)
    out += sound_table + bytes(sound_blob)

    os.makedirs(os.path.dirname(a.out) or '.', exist_ok=True)
    open(a.out, 'wb').write(out)

    with open(a.header, 'w', newline='\n') as f:
        f.write('/* Generated by tools/mkassets.py. DO NOT EDIT.\n'
                ' * Asset ids for SD:\\Aurora\\assets.pak; see docs/assets.md. */\n'
                '#ifndef ASSET_IDS_H\n#define ASSET_IDS_H\n\n')
        for i, n in enumerate(ids):
            f.write('#define ASSET_%-26s %du\n' % (n, i))
        f.write('\n#define ASSET_COUNT %du\n' % len(ids))
        f.write('#define ASSET_ARENA_NEEDED %du\n' % arena)
        f.write('#define ASSET_MAX_CSIZE %du\n\n' % max_csize)
        for i, (n, _, _) in enumerate(SOUNDS):
            f.write('#define ASSET_%-26s %du\n' % ('SOUND_' + n, i))
        f.write('\n#define ASSET_SOUND_COUNT %du\n\n#endif\n' % len(SOUNDS))

    raw = sum(len(e[4]) for e in entries)
    print(
        '%s: %d assets, %d bytes on disk (%d decoded, %.0f%% saved), '
        '%d sounds in %d bytes'
        % (a.out, len(entries), len(out), arena,
           100.0 * (1 - len(blob) / float(raw)), len(SOUNDS), len(sound_blob))
    )
    print('%s: ASSET_COUNT=%d ASSET_ARENA_NEEDED=%d' % (a.header, len(ids), arena))


if __name__ == '__main__':
    main()
