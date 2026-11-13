import argparse
import binascii
import struct
import sys
from pathlib import Path

CREDIT = b'Cracko298\x00\x02\x98'
SEED = binascii.crc32(b'Cracko298')
ICON_BYTES = 2056


def check_banner(bnr):
    if len(bnr) < 32 or bnr[:3] != b'BNR':
        raise ValueError('not a .bnr banner')
    if bnr[3] != 3:
        raise ValueError(f'banner version {bnr[3]}: rebuild it with BNR Studio (version 3 is needed)')
    if bnr[4:16] != CREDIT:
        raise ValueError('the Cracko298 seal is missing or altered')
    size, count, _, first, crc = struct.unpack_from('<IHHII', bnr, 16)
    if size != len(bnr):
        raise ValueError(f'the banner says {size} bytes but the file has {len(bnr)}')
    body = bytearray(bnr)
    body[28:32] = b'\0\0\0\0'
    if binascii.crc32(bytes(body), SEED) & 0xFFFFFFFF != crc:
        raise ValueError('the banner checksum is wrong')
    icon = None
    pos = first
    for _ in range(count):
        tag, ln = struct.unpack_from('<4sI', bnr, pos)
        data = bnr[pos + 8:pos + 8 + ln]
        if tag == b'ICON' and data[:1] == b'\x02' and data[1:5] == b'AIC1' and len(data) >= 1 + ICON_BYTES:
            icon = bytes(data[1:1 + ICON_BYTES])
        pos += 8 + ln
    return icon


def split_app(app):
    if len(app) < 36 or app[:4] not in (b'AURC', b'AUR1', b'AOS1'):
        raise ValueError('not an Aurora app container (AURC, AUR1 or AOS1)')
    end = 36 + struct.unpack_from('<I', app, 8)[0]
    if end > len(app):
        raise ValueError('the container is shorter than its header says')
    rest = app[end:]
    icon = rest[:ICON_BYTES] if rest[:4] == b'AIC1' and len(rest) >= ICON_BYTES else None
    return app[:end], icon


def embed(app, bnr, keep_app_icon=False):
    banner_icon = check_banner(bnr)
    payload, app_icon = split_app(app)
    icon = app_icon if keep_app_icon and app_icon else (banner_icon or app_icon)
    if icon is None:
        raise ValueError('neither the app nor the banner has an Aurora colour icon (AIC1)')
    return payload + icon + bnr


def strip(app):
    payload, icon = split_app(app)
    return payload + (icon or b'')


def main(argv=None):
    ap = argparse.ArgumentParser(description='Puts a .bnr banner inside an Aurora app, after its colour icon, as one file.')
    ap.add_argument('app', help='the app container, e.g. NovaLauncher.bin')
    ap.add_argument('banner', nargs='?', help='the .bnr banner (omit with --strip)')
    ap.add_argument('-o', '--output', help='where to write the result (default: the app, in place)')
    ap.add_argument('--keep-app-icon', action='store_true', help="keep the app's own AIC1 icon instead of the banner's")
    ap.add_argument('--strip', action='store_true', help='remove an embedded banner')
    a = ap.parse_args(argv)
    app = Path(a.app).read_bytes()
    try:
        if a.strip:
            out = strip(app)
        else:
            if not a.banner:
                ap.error('a banner is needed unless --strip is given')
            out = embed(app, Path(a.banner).read_bytes(), a.keep_app_icon)
    except ValueError as e:
        print(f'error: {e}', file=sys.stderr)
        return 1
    dest = Path(a.output or a.app)
    dest.write_bytes(out)
    payload, _ = split_app(out)
    extra = len(out) - len(payload)
    print(f'{dest}: {len(out):,} bytes ({len(payload):,} app, {extra:,} icon and banner)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
