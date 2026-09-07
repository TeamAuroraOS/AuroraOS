"""Extract the embedded Atheros Wi-Fi firmware blocks from the 3DS NWM module.

Extracts the six blobs needed to boot the AR6014: stub_data, stub_code, database,
and Main.type1/4/5. They are located via the firmware-address pool (marker
0x00524C00, verify 0x000003ED) whose entries are (end, start) runtime-address
pairs; file_off = addr - 0x100000.

Usage:  python tools/nwm_extract.py <nwm.dec.code> [out_dir]
"""
import struct
import sys
from pathlib import Path

LOAD_BASE = 0x00100000
POOL_MARKER = 0x00524C00
POOL_VERIFY = 0x000003ED

# GBATEK/Octoblimp-documented sizes, for a sanity check.
KNOWN_SIZES = {
    "stub_data": 56,
    "stub_code": 790,
    "database": 488,
    "Main.type1": 0x1B1B,
    "Main.type4": 0xA5EB,
    "Main.type5": 0x7A2E,
}

# 8.3 output filenames. AuroraOS's FatFs is built without long-filename support,
# so the SD copies must be plain 8.3 names to be openable. Copy these to
# SD:/Aurora/wifi/. The boot path uses only stub_data, stub_code, main_type4,
# and database; type1/type5 are extracted for completeness.
SD_NAMES = {
    "stub_data": "STUBDATA.BIN",
    "stub_code": "STUBCODE.BIN",
    "database": "DATABASE.BIN",
    "Main.type4": "MAINTYP4.BIN",
    "Main.type1": "MAINTYP1.BIN",
    "Main.type5": "MAINTYP5.BIN",
}


def find_pool(data: bytes) -> int:
    """Offset of the pool that also carries the stub/database descriptors.

    Two pools carry the marker; the one we want has valid runtime addresses in
    the words after the three main (end, start) pairs (word index 8+).
    """
    needle = struct.pack("<I", POOL_MARKER)
    i = 0
    fallback = -1
    while True:
        j = data.find(needle, i)
        if j < 0:
            break
        if struct.unpack_from("<I", data, j + 4)[0] == POOL_VERIFY:
            if fallback < 0:
                fallback = j
            w8 = struct.unpack_from("<I", data, j + 8 * 4)[0]
            if LOAD_BASE <= w8 < 0x00200000:
                return j
        i = j + 1
    if fallback < 0:
        raise SystemExit("firmware pool marker 0x00524C00 not found")
    return fallback


def main() -> None:
    if not (2 <= len(sys.argv) <= 3):
        raise SystemExit("usage: nwm_extract.py <nwm.dec.code> [out_dir]")
    data = Path(sys.argv[1]).read_bytes()
    out = Path(sys.argv[2] if len(sys.argv) == 3 else "fw")
    out.mkdir(parents=True, exist_ok=True)

    pool = find_pool(data)
    w = struct.unpack_from("<24I", data, pool)  # marker, verify, then descriptors
    # (name, end_word_index, start_word_index)
    blocks = [
        ("Main.type1", 2, 3),
        ("Main.type5", 6, 7),
        ("Main.type4", 4, 5),
        ("database", 8, 9),
        ("stub_code", 11, 12),
        ("stub_data", 14, 15),
    ]
    print(f"pool at file offset 0x{pool:X}")
    for name, ei, si in blocks:
        s = w[si] - LOAD_BASE
        e = w[ei] - LOAD_BASE
        size = e - s
        ok = "MATCH" if size == KNOWN_SIZES.get(name) else "MISMATCH"
        fname = SD_NAMES[name]
        (out / fname).write_bytes(data[s:e])
        print(f"  {name:<11} [0x{s:05X}..0x{e:05X}) size 0x{size:X} ({size}) "
              f"[{ok}] -> {out / fname}")


if __name__ == "__main__":
    main()
