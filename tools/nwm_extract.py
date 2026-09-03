"""Extract the embedded Atheros Wi-Fi firmware blocks from the 3DS NWM module.

Usage:  python tools/nwm_extract.py <nwm.dec.code> [out_dir]
"""
import struct
import sys
from pathlib import Path

LOAD_BASE = 0x00100000
POOL_MARKER = 0x00524C00
POOL_VERIFY = 0x000003ED

# GBATEK-documented sizes, for a sanity check.
GBATEK_SIZES = {
    "Main.type1": (0x0FD3, 0x10F7, 0x1B1B),
    "Main.type4": (0xA053, 0xA482, 0xA5EB),
    "Main.type5": (0x78F6, 0x7A2E),
}


def find_pool(data: bytes) -> int:
    """Offset of the firmware-address pool (marker 0x00524C00, verify 0x3ED)."""
    needle = struct.pack("<I", POOL_MARKER)
    i = 0
    while True:
        j = data.find(needle, i)
        if j < 0:
            raise SystemExit("firmware pool marker 0x00524C00 not found")
        if struct.unpack_from("<I", data, j + 4)[0] == POOL_VERIFY:
            return j
        i = j + 1


def main() -> None:
    if not (2 <= len(sys.argv) <= 3):
        raise SystemExit("usage: nwm_extract.py <nwm.dec.code> [out_dir]")
    data = Path(sys.argv[1]).read_bytes()
    out = Path(sys.argv[2] if len(sys.argv) == 3 else "fw")
    out.mkdir(parents=True, exist_ok=True)

    pool = find_pool(data)
    p = struct.unpack_from("<8I", data, pool)  # marker, verify, then 3 (end,start) pairs
    blocks = [
        ("Main.type1", p[3] - LOAD_BASE, p[2] - LOAD_BASE),
        ("Main.type5", p[7] - LOAD_BASE, p[6] - LOAD_BASE),
        ("Main.type4", p[5] - LOAD_BASE, p[4] - LOAD_BASE),
    ]
    print(f"pool at file offset 0x{pool:X}")
    for name, s, e in blocks:
        size = e - s
        ok = "MATCH" if size in GBATEK_SIZES[name] else "MISMATCH"
        (out / f"{name}.bin").write_bytes(data[s:e])
        print(f"  {name:<11} [0x{s:05X}..0x{e:05X}) size 0x{size:X} ({size}) [{ok}]"
              f" -> {out / (name + '.bin')}")


if __name__ == "__main__":
    main()
