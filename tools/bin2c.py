"""Emit a C array for a binary file: `python bin2c.py <in.bin> <varname> > out.h`.

Used by the OS build to embed the ARM11 audio core (audio11.bin) into the ARM9
OS as a byte blob, so the OS can copy it into place and wake the ARM11 itself.
"""
import sys
from pathlib import Path


def main() -> None:
    if len(sys.argv) != 3:
        print("usage: bin2c.py <input.bin> <varname>", file=sys.stderr)
        sys.exit(1)
    data = Path(sys.argv[1]).read_bytes()
    name = sys.argv[2]
    out = [f"/* Auto-generated from {sys.argv[1]} by tools/bin2c.py. Do not edit. */"]
    out.append(f"static const unsigned int {name}_len = {len(data)};")
    out.append(f"static const unsigned char {name}[] = {{")
    for i in range(0, len(data), 12):
        chunk = data[i : i + 12]
        out.append("  " + "".join(f"0x{b:02x}," for b in chunk))
    out.append("};")
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
