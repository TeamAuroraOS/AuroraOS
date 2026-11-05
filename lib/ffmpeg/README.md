# FFmpeg, for the video engine

The H.264 and AAC decoders from [FFmpeg](https://ffmpeg.org/) 7.1.1, cut down to
what the video engine (`sdk/video11`) links. They run on the ARM11 cores and nowhere
else, and only in apps built with `aurcc build --video`.

**License: LGPL version 2.1 or later** (`COPYING.LGPLv2.1`, `LICENSE.md`,
`CREDITS`). No GPL-only FFmpeg part is included. An app that links this has to let
its users relink it against a changed FFmpeg, which building from this source tree
allows.

H.264 and AAC are covered by patents in some countries, whatever the code's
license. FFmpeg's own [legal page](https://ffmpeg.org/legal.html) has more on what
that means for anyone distributing it.

## Where it came from

`ffmpeg-7.1.1.tar.xz` from ffmpeg.org, SHA-256
`733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1`, with a good GPG
signature from the FFmpeg release signing key,
`FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8`.

`config.h`, `config_components.h`, `libavutil/avconfig.h` and the
`libavcodec/*_list.c` files are what its `configure` wrote for:

```
--enable-cross-compile --cross-prefix=arm-none-eabi- --arch=arm --cpu=armv6k
--target-os=none --disable-all --disable-autodetect --disable-doc --disable-debug
--disable-network --disable-iconv --disable-runtime-cpudetect --disable-neon
--disable-vfpv3 --enable-vfp --enable-armv5te --enable-armv6 --disable-armv6t2
--disable-thumb --enable-inline-asm --enable-avcodec --enable-avutil
--enable-decoder=h264 --enable-decoder=aac --enable-decoder=aac_fixed
```

`sources.txt` lists the files compiled: those whose code survives the engine's link
with `--gc-sections`. FFmpeg's Makefiles were read for the list but not run, and the
engine is built by `sdk/video11/build.py`, which only calls the compiler.

## Local changes

- Non-ASCII characters, all of them in comments and names, were reduced to ASCII,
  as everywhere in this tree.
- `libavutil/tx.c`: only the float transforms are listed, and
  `libavutil/tx_template.c` stops at 8192 points. FFmpeg's tables for the sizes up to
  2,097,152, in three number formats, are 17 MB of `.bss`, more than an Old 3DS has
  for the whole engine; AAC needs none of them.
- `config_components.h` and `libavcodec/codec_list.c`: `aac_fixed` is off, so there
  is one AAC decoder.
- `libavutil/ffversion.h` is written by FFmpeg's build; here it is a file.

The engine supplies `pthread_*` and newlib's locks itself
(`sdk/video11/threads11.c`), since nothing here has a scheduler.
