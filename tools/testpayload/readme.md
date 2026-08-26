# Phase 7 test payload

The smallest possible AuroraOS payload: a standalone ARM9 binary that fills both
screens solid green and spins. Booting it via "Boot Aurora" proves the whole
loader pipeline (SD read → FAT → copy to `0x22000000` → cache flush → jump) works,
isolated from any real OS code.

- `payload.s` — the ARM9 code (fills `0x18300000` for both framebuffers).
- `payload.ld` — links it to run in place at `0x22000000` with `_start` first.

## Build + package

```
arm-none-eabi-gcc -mcpu=arm946e-s -march=armv5te -marm -c tools/testpayload/payload.s -o build/payload.o
arm-none-eabi-gcc -T tools/testpayload/payload.ld -nostdlib -nostartfiles -Wl,--build-id=none build/payload.o -o build/payload.elf
arm-none-eabi-objcopy -O binary build/payload.elf build/payload.bin
python tools/aos_pack.py pack build/payload.bin -o output/AURORAOS.BIN --arm9-load 0x22000000
```

Copy `output/AURORAOS.BIN` to the **root of the SD card**, boot the Aurora firm,
and pick **Boot** on the home screen. Green screens = success.
