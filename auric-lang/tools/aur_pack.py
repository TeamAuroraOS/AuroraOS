"""Pack/inspect Auric "AUR1" bootable containers.

Forked from AuroraOS's tools/aos_pack.py. An AUR1 file has the *identical*
36-byte little-endian header layout as AOS1 (see ../../include/loader.h); only
the 4-byte magic differs. Auric programs are ARM9-only for v1, so the four
ARM11 header fields are always zero.

    [ 36-byte header ][ arm9 payload ]

    char     magic[4]        "AUR1"
    uint32_t arm9_offset     offset of the ARM9 payload in the file
    uint32_t arm9_size
    uint32_t arm9_load_addr  where the loader copies the ARM9 payload
    uint32_t arm9_entry      where the loader branches on ARM9
    uint32_t arm11_offset    always 0 (no ARM11 payload)
    uint32_t arm11_size      always 0
    uint32_t arm11_load_addr always 0
    uint32_t arm11_entry     always 0

Note on booting: AuroraOS's stock loader (boot_aurora) hard-checks for "AOS1"
magic, so an AUR1 file renamed to AURORAOS.BIN will not boot on the *unmodified*
loader. The layout is byte-identical, so either:
  * pack with `--magic AOS1` for a standalone drop-in boot test on the stock
    loader (proves the pipeline end-to-end), or
  * use the AUR1 default once AuroraOS is updated (Part 3) to accept both magics.
"""
import argparse
import struct
import sys
from pathlib import Path

MAGIC_AUR1 = b"AUR1"
MAGIC_AOS1 = b"AOS1"
KNOWN_MAGICS = (MAGIC_AUR1, MAGIC_AOS1)

HEADER_FMT = "<4sIIIIIIII"  # 4 + 8*4 = 36 bytes
HEADER_SIZE = struct.calcsize(HEADER_FMT)
assert HEADER_SIZE == 36

# Memory the running loader occupies (from ../../arm9.ld). A loaded payload must
# not sit here or it clobbers the code doing the copy. Soft warning only.
LOADER_ARM9_RANGE = (0x08006800, 0x08100000)

# Default ARM9 payload address in FCRAM, well clear of the loader -- the same
# address AuroraOS itself is packed at.
DEFAULT_ARM9_LOAD = 0x22000000


def _overlaps(addr: int, size: int, region: tuple[int, int]) -> bool:
    if size == 0:
        return False
    lo, hi = region
    return addr < hi and (addr + size) > lo


def pack(
    arm9_path: Path,
    output_path: Path,
    arm9_load: int,
    arm9_entry: int,
    magic: bytes = MAGIC_AUR1,
) -> None:
    arm9 = arm9_path.read_bytes()
    if not arm9:
        raise ValueError(f"{arm9_path} is empty")

    arm9_offset = HEADER_SIZE
    arm9_size = len(arm9)

    if _overlaps(arm9_load, arm9_size, LOADER_ARM9_RANGE):
        print(
            f"warning: ARM9 payload [{arm9_load:#010x}..{arm9_load + arm9_size:#010x}] "
            f"overlaps the loader's ARM9 region {LOADER_ARM9_RANGE[0]:#010x}.."
            f"{LOADER_ARM9_RANGE[1]:#010x}",
            file=sys.stderr,
        )

    header = struct.pack(
        HEADER_FMT,
        magic,
        arm9_offset,
        arm9_size,
        arm9_load,
        arm9_entry,
        0,  # arm11_offset
        0,  # arm11_size
        0,  # arm11_load_addr
        0,  # arm11_entry
    )

    with open(output_path, "wb") as f:
        f.write(header)
        f.write(arm9)

    total = HEADER_SIZE + arm9_size
    print(f"Wrote {output_path} ({total} bytes, magic {magic.decode('ascii')})")
    print(f"  ARM9 : offset {arm9_offset:#x}  size {arm9_size}  "
          f"load {arm9_load:#010x}  entry {arm9_entry:#010x}")
    print("  ARM11: (none)")


def info(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < HEADER_SIZE or data[:4] not in KNOWN_MAGICS:
        raise ValueError(f"{path} is not an AUR1/AOS1 container (bad magic)")

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

    if arm9_offset + arm9_size > len(data):
        print("  ERROR: ARM9 payload runs past end of file", file=sys.stderr)


def _auto(value: str) -> int:
    """Parse a decimal or 0x-prefixed hex integer."""
    return int(value, 0)


def _magic(value: str) -> bytes:
    b = value.encode("ascii")
    if len(b) != 4:
        raise argparse.ArgumentTypeError("magic must be exactly 4 ASCII chars")
    return b


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Pack/inspect Auric AUR1 bootable containers."
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_pack = sub.add_parser("pack", help="Build an AUR1 container")
    p_pack.add_argument("arm9", type=Path, help="ARM9 payload binary")
    p_pack.add_argument("-o", "--output", type=Path, default=Path("program.bin"),
                        help="Output file (default: program.bin)")
    p_pack.add_argument("--arm9-load", type=_auto, default=DEFAULT_ARM9_LOAD,
                        help=f"ARM9 load address (default: {DEFAULT_ARM9_LOAD:#x})")
    p_pack.add_argument("--arm9-entry", type=_auto, default=None,
                        help="ARM9 entry address (default: = load address)")
    p_pack.add_argument("--magic", type=_magic, default=MAGIC_AUR1,
                        help="Container magic: AUR1 (default) or AOS1 (for a "
                             "drop-in boot test on the unmodified loader)")

    p_info = sub.add_parser("info", help="Print the header of an AUR1/AOS1 container")
    p_info.add_argument("input", type=Path, help="Container file to inspect")

    args = parser.parse_args()

    try:
        if args.command == "pack":
            arm9_entry = args.arm9_entry if args.arm9_entry is not None else args.arm9_load
            pack(args.arm9, args.output, args.arm9_load, arm9_entry, args.magic)
        elif args.command == "info":
            info(args.input)
    except (ValueError, FileNotFoundError, OSError) as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
