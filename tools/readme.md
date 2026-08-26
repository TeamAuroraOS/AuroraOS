# aos_pack.py — AuroraOS container packer

Builds and inspects **AOS1** bootable containers, the format the AuroraOS
loader ("Boot Aurora") reads off the SD card. Layout:

```
[ 36-byte header ][ arm9 payload ][ arm11 payload ]
```

The header mirrors `aos_header_t` in `include/loader.h` (little-endian). Keep the
two in sync — the loader parses exactly these fields.

## Usage

Pack an ARM9 + ARM11 pair:

```
python tools/aos_pack.py pack arm9.bin arm11.bin -o aurora_os.bin \
    --arm9-load 0x22000000 --arm9-entry 0x22000000 \
    --arm11-load 0x24000000 --arm11-entry 0x24000000
```

The ARM11 payload is optional (Phase 7's test payload uses none):

```
python tools/aos_pack.py pack arm9.bin -o aurora_os.bin --arm9-load 0x22000000
```

Entry addresses default to the matching load address. The load-address defaults
are provisional (FCRAM, clear of the running loader); **Phase 4 finalizes them**.
The packer prints a warning if a payload would overlap the loader's own ARM9
(`0x08006800..0x08100000`) or ARM11 (`0x1FF80000..0x20000000`) memory.

Inspect a container:

```
python tools/aos_pack.py info aurora_os.bin
```

The final loader expects the file on the SD root named **`AURORAOS.BIN`** (8.3,
because FatFs is built without long file names).
