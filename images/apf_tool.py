import argparse
import struct
import sys
from pathlib import Path

from PIL import Image

MAGIC = b"APF1"
TARGET_WIDTH = 400
TARGET_HEIGHT = 240
TARGET_RATIO = TARGET_WIDTH / TARGET_HEIGHT
RATIO_TOLERANCE = 0.01

FORMAT_RGB = 0
FORMAT_RGBA = 1
HEADER_SIZE = 10


def check_aspect_ratio(width: int, height: int) -> None:
    """Raise ValueError if width:height doesn't match the 400:240 (5:3) target."""
    ratio = width / height
    if abs(ratio - TARGET_RATIO) > RATIO_TOLERANCE:
        raise ValueError(
            f"Image is {width}x{height} (ratio {ratio:.4f}). That doesn't match "
            f"the required {TARGET_WIDTH}:{TARGET_HEIGHT} ratio ({TARGET_RATIO:.4f}). "
            "Crop it to a 5:3 aspect ratio first, or pick a different image."
        )


def encode(input_path: Path, output_path: Path, keep_alpha: bool = False) -> None:
    """Convert a PNG/JPEG (or anything Pillow can open) to a .apf file."""
    img = Image.open(input_path)
    check_aspect_ratio(*img.size)

    mode = "RGBA" if keep_alpha else "RGB"
    img = img.convert(mode)
    img = img.resize((TARGET_WIDTH, TARGET_HEIGHT), Image.LANCZOS)

    pixel_format = FORMAT_RGBA if keep_alpha else FORMAT_RGB
    header = MAGIC + struct.pack("<HHBB", TARGET_WIDTH, TARGET_HEIGHT, pixel_format, 0)

    with open(output_path, "wb") as f:
        f.write(header)
        f.write(img.tobytes())

    print(f"Wrote {output_path} ({TARGET_WIDTH}x{TARGET_HEIGHT}, {mode})")


def decode(apf_path: Path) -> Image.Image:
    """Read a .apf file and return a Pillow Image."""
    data = Path(apf_path).read_bytes()

    if len(data) < HEADER_SIZE or data[:4] != MAGIC:
        raise ValueError(f"{apf_path} is not a valid .apf file (bad magic bytes).")

    width, height, pixel_format, _reserved = struct.unpack("<HHBB", data[4:HEADER_SIZE])
    if pixel_format not in (FORMAT_RGB, FORMAT_RGBA):
        raise ValueError(f"{apf_path} has an unrecognized pixel format ({pixel_format}).")

    mode = "RGBA" if pixel_format == FORMAT_RGBA else "RGB"
    channels = 4 if pixel_format == FORMAT_RGBA else 3
    expected_len = width * height * channels

    pixel_data = data[HEADER_SIZE:HEADER_SIZE + expected_len]
    if len(pixel_data) != expected_len:
        raise ValueError(
            f"{apf_path} is truncated or corrupt: expected {expected_len} bytes "
            f"of pixel data, got {len(pixel_data)}."
        )

    return Image.frombytes(mode, (width, height), pixel_data)


def main() -> None:
    parser = argparse.ArgumentParser(description="Convert to/from the custom .apf image format.")
    sub = parser.add_subparsers(dest="command", required=True)

    p_convert = sub.add_parser("convert", help="Convert a PNG/JPEG to .apf")
    p_convert.add_argument("input", type=Path, help="Source .png or .jpg/.jpeg file")
    p_convert.add_argument("output", type=Path, nargs="?", help="Output .apf path (default: same name, .apf extension)")
    p_convert.add_argument("--alpha", action="store_true", help="Keep the alpha channel (RGBA instead of RGB)")

    p_open = sub.add_parser("open", help="Open/view a .apf file")
    p_open.add_argument("input", type=Path, help="Source .apf file")
    p_open.add_argument("--save", type=Path, help="Save as a normal image (e.g. output.png) instead of displaying it")

    args = parser.parse_args()

    if args.command == "convert":
        output = args.output or args.input.with_suffix(".apf")
        try:
            encode(args.input, output, keep_alpha=args.alpha)
        except (ValueError, FileNotFoundError) as e:
            print(f"Error: {e}", file=sys.stderr)
            sys.exit(1)

    elif args.command == "open":
        try:
            img = decode(args.input)
        except (ValueError, FileNotFoundError) as e:
            print(f"Error: {e}", file=sys.stderr)
            sys.exit(1)

        if args.save:
            img.save(args.save)
            print(f"Saved {args.save}")
        else:
            img.show()


if __name__ == "__main__":
    main()