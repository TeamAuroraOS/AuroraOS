"""Pack/inspect AuroraOS "AOS1" bootable containers.

An AOS1 file is just:

    [ aos_header_t ][ arm9 payload ][ arm11 payload ]

The 36-byte header (little-endian) mirrors `aos_header_t` in include/loader.h:

    char     magic[4]        "AOS1"
    uint32_t arm9_offset     offset of the ARM9 payload in the file
    uint32_t arm9_size
    uint32_t arm9_load_addr  where the loader copies the ARM9 payload
    uint32_t arm9_entry      where the loader branches on ARM9
    uint32_t arm11_offset    0 if there is no ARM11 payload
    uint32_t arm11_size      0 if there is no ARM11 payload
    uint32_t arm11_load_addr
    uint32_t arm11_entry
"""
import argparse
import struct
import sys
from pathlib import Path

MAGIC = b"AOS1"
HEADER_FMT = "<4sIIIIIIII"  # 4 + 8*4 = 36 bytes
HEADER_SIZE = struct.calcsize(HEADER_FMT)
assert HEADER_SIZE == 36

# Memory regions the running loader itself occupies (from arm9.ld / arm11.ld).
# A loaded payload must NOT be placed on top of these, or it clobbers the code
# doing the copy. Used only for a soft warning.
LOADER_ARM9_RANGE = (0x08006800, 0x08100000)
LOADER_ARM11_RANGE = (0x1FF80000, 0x20000000)

# Default payload addresses in FCRAM, well clear of the loader.
DEFAULT_ARM9_LOAD = 0x22000000
DEFAULT_ARM11_LOAD = 0x24000000


def _overlaps(addr: int, size: int, region: tuple[int, int]) -> bool:
    if size == 0:
        return False
    lo, hi = region
    return addr < hi and (addr + size) > lo


def pack(
    arm9_path: Path,
    arm11_path: Path | None,
    output_path: Path,
    arm9_load: int,
    arm9_entry: int,
    arm11_load: int,
    arm11_entry: int,
) -> None:
    arm9 = arm9_path.read_bytes()
    if not arm9:
        raise ValueError(f"{arm9_path} is empty")
    arm11 = arm11_path.read_bytes() if arm11_path else b""

    arm9_offset = HEADER_SIZE
    arm9_size = len(arm9)
    if arm11:
        arm11_offset = HEADER_SIZE + arm9_size
        arm11_size = len(arm11)
    else:
        arm11_offset = 0
        arm11_size = 0
        arm11_load = 0
        arm11_entry = 0

    # Soft safety check against the running loader's own memory.
    if _overlaps(arm9_load, arm9_size, LOADER_ARM9_RANGE):
        print(
            f"warning: ARM9 payload [{arm9_load:#010x}..{arm9_load + arm9_size:#010x}] "
            f"overlaps the loader's ARM9 region {LOADER_ARM9_RANGE[0]:#010x}.."
            f"{LOADER_ARM9_RANGE[1]:#010x}",
            file=sys.stderr,
        )
    if _overlaps(arm11_load, arm11_size, LOADER_ARM11_RANGE):
        print(
            f"warning: ARM11 payload [{arm11_load:#010x}..{arm11_load + arm11_size:#010x}] "
            f"overlaps the loader's ARM11 region {LOADER_ARM11_RANGE[0]:#010x}.."
            f"{LOADER_ARM11_RANGE[1]:#010x}",
            file=sys.stderr,
        )

    header = struct.pack(
        HEADER_FMT,
        MAGIC,
        arm9_offset,
        arm9_size,
        arm9_load,
        arm9_entry,
        arm11_offset,
        arm11_size,
        arm11_load,
        arm11_entry,
    )

    with open(output_path, "wb") as f:
        f.write(header)
        f.write(arm9)
        f.write(arm11)

    total = HEADER_SIZE + arm9_size + arm11_size
    print(f"Wrote {output_path} ({total} bytes)")
    print(f"  ARM9 : offset {arm9_offset:#x}  size {arm9_size}  "
          f"load {arm9_load:#010x}  entry {arm9_entry:#010x}")
    if arm11_size:
        print(f"  ARM11: offset {arm11_offset:#x}  size {arm11_size}  "
              f"load {arm11_load:#010x}  entry {arm11_entry:#010x}")
    else:
        print("  ARM11: (none)")


def info(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < HEADER_SIZE or data[:4] != MAGIC:
        raise ValueError(f"{path} is not an AOS1 container (bad magic)")

    (
        magic,
        arm9_offset,
        arm9_size,
        arm9_load,
        arm9_entry,
        arm11_offset,
        arm11_size,
        arm11_load,
        arm11_entry,
    ) = struct.unpack(HEADER_FMT, data[:HEADER_SIZE])

    print(f"{path}: {magic.decode('ascii', 'replace')} container, {len(data)} bytes")
    print(f"  ARM9 : offset {arm9_offset:#x}  size {arm9_size}  "
          f"load {arm9_load:#010x}  entry {arm9_entry:#010x}")
    if arm11_size:
        print(f"  ARM11: offset {arm11_offset:#x}  size {arm11_size}  "
              f"load {arm11_load:#010x}  entry {arm11_entry:#010x}")
    else:
        print("  ARM11: (none)")

    # Basic integrity checks.
    if arm9_offset + arm9_size > len(data):
        print("  ERROR: ARM9 payload runs past end of file", file=sys.stderr)
    if arm11_size and arm11_offset + arm11_size > len(data):
        print("  ERROR: ARM11 payload runs past end of file", file=sys.stderr)


def _auto(value: str) -> int:
    """Parse a decimal or 0x-prefixed hex integer."""
    return int(value, 0)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Pack/inspect AuroraOS AOS1 bootable containers."
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_pack = sub.add_parser("pack", help="Build an AOS1 container")
    p_pack.add_argument("arm9", type=Path, help="ARM9 payload binary")
    p_pack.add_argument("arm11", type=Path, nargs="?", help="ARM11 payload binary (optional)")
    p_pack.add_argument("-o", "--output", type=Path, default=Path("aurora_os.bin"),
                        help="Output file (default: aurora_os.bin)")
    p_pack.add_argument("--arm9-load", type=_auto, default=DEFAULT_ARM9_LOAD,
                        help=f"ARM9 load address (default: {DEFAULT_ARM9_LOAD:#x})")
    p_pack.add_argument("--arm9-entry", type=_auto, default=None,
                        help="ARM9 entry address (default: = load address)")
    p_pack.add_argument("--arm11-load", type=_auto, default=DEFAULT_ARM11_LOAD,
                        help=f"ARM11 load address (default: {DEFAULT_ARM11_LOAD:#x})")
    p_pack.add_argument("--arm11-entry", type=_auto, default=None,
                        help="ARM11 entry address (default: = load address)")

    p_info = sub.add_parser("info", help="Print the header of an AOS1 container")
    p_info.add_argument("input", type=Path, help="AOS1 file to inspect")

    args = parser.parse_args()

    try:
        if args.command == "pack":
            arm9_entry = args.arm9_entry if args.arm9_entry is not None else args.arm9_load
            arm11_entry = args.arm11_entry if args.arm11_entry is not None else args.arm11_load
            pack(args.arm9, args.arm11, args.output,
                 args.arm9_load, arm9_entry, args.arm11_load, arm11_entry)
        elif args.command == "info":
            info(args.input)
    except (ValueError, FileNotFoundError, OSError) as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
