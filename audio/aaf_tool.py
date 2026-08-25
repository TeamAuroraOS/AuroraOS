# Coded By DisLoPik for the AuroraOS Project.
import argparse
import struct
from pathlib import Path

MAGIC = b"AAF1"
VERSION = 1
HEADER_FORMAT = "<4sBBBBII"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)


def convert_to_aaf(input_path: str, output_path: str, sample_rate: int, bit_depth: int = 16):
    from pydub import AudioSegment

    print(f"Loading {input_path} ...")
    audio = AudioSegment.from_file(input_path)

    print(f"Converting to mono, {sample_rate} Hz, {bit_depth}-bit ...")
    audio = audio.set_channels(1)
    audio = audio.set_frame_rate(sample_rate)
    audio = audio.set_sample_width(bit_depth // 8)

    pcm_data = audio.raw_data
    num_samples = len(pcm_data) // (bit_depth // 8)

    header = struct.pack(
        HEADER_FORMAT,
        MAGIC,
        VERSION,
        1,
        bit_depth,
        0,
        sample_rate,
        num_samples,
    )

    with open(output_path, "wb") as f:
        f.write(header)
        f.write(pcm_data)

    orig_size = Path(input_path).stat().st_size
    new_size = Path(output_path).stat().st_size
    print(f"Wrote {output_path} ({new_size:,} bytes, was {orig_size:,} bytes)")


def read_aaf(path: str):
    with open(path, "rb") as f:
        header_bytes = f.read(HEADER_SIZE)
        if len(header_bytes) < HEADER_SIZE:
            raise ValueError("File too small to be a valid .aaf file")

        magic, version, channels, bit_depth, _reserved, sample_rate, num_samples = struct.unpack(
            HEADER_FORMAT, header_bytes
        )

        if magic != MAGIC:
            raise ValueError(f"Not a valid .aaf file (bad magic: {magic!r})")

        pcm_data = f.read(num_samples * (bit_depth // 8))

    return {
        "version": version,
        "channels": channels,
        "bit_depth": bit_depth,
        "sample_rate": sample_rate,
        "num_samples": num_samples,
        "pcm_data": pcm_data,
    }


def play_aaf(path: str):
    import simpleaudio as sa

    info = read_aaf(path)
    print(
        f"Playing {path}: {info['sample_rate']} Hz, "
        f"{info['bit_depth']}-bit, mono, {info['num_samples']} samples"
    )

    play_obj = sa.play_buffer(
        info["pcm_data"],
        num_channels=info["channels"],
        bytes_per_sample=info["bit_depth"] // 8,
        sample_rate=info["sample_rate"],
    )
    play_obj.wait_done()


def main():
    parser = argparse.ArgumentParser(description="Convert to / play .aaf audio files")
    subparsers = parser.add_subparsers(dest="command", required=True)

    convert_parser = subparsers.add_parser("convert", help="Convert an audio file to .aaf")
    convert_parser.add_argument("input", help="Input audio file (mp3, wav, etc.)")
    convert_parser.add_argument("output", help="Output .aaf file path")
    convert_parser.add_argument(
        "--rate", type=int, default=8000,
        help="Sample rate in Hz (default: 8000 -- low, for small file size)"
    )
    convert_parser.add_argument(
        "--bit-depth", type=int, default=16, choices=[8, 16],
        help="Bit depth (default: 16)"
    )

    play_parser = subparsers.add_parser("play", help="Play a .aaf file")
    play_parser.add_argument("input", help="Input .aaf file")

    args = parser.parse_args()

    if args.command == "convert":
        convert_to_aaf(args.input, args.output, args.rate, args.bit_depth)
    elif args.command == "play":
        play_aaf(args.input)


if __name__ == "__main__":
    main()