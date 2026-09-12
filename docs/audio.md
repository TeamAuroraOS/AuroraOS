# Audio (CSND)

Sound is played by the CSND mixer at `0x10103000`, which lives on the ARM11. The
OS runs on the ARM9, so playback is driven from the ARM11 core through the
command block described in `include/audio.h`.

| Piece | File |
|-------|------|
| ARM9 API, core boot | `src/os/Audio9.c` |
| Output path + CSND | `src/os/Audio11.c` |
| Codec bus (shared with the touchscreen) | `src/os/Codec11.c` |

## The channel register layout

This is the part worth writing down, because the obvious reference is
misleading. libctru's `csnd.h` packs a channel setup as

```c
flags | SOUND_ENABLE | (CSND_TIMER(rate) << 16)
```

That word is the **argument to the CSND system module**, which decomposes it
before touching hardware. It is not the register layout. On bare metal, writing
it straight to the channel register does not work.

Channel `n` occupies `0x10103400 + n*0x20`:

| Offset | Width | Meaning | Notes |
|--------|-------|---------|-------|
| `+0x00` | 16 | Control flags | **bit 15 starts the channel** |
| `+0x02` | 16 | Period reload | **write-only**, and **two's complement** |
| `+0x04` | 32 | Volume | right in the low half, left in the high |
| `+0x0C` | 32 | Source address | physical |
| `+0x10` | 32 | Length in bytes | |
| `+0x14` | 32 | Loop address | physical |

Control-flag bits match libctru's `SOUND_*` values: bit 6 linear interpolation,
bits 10-11 loop mode (1 = loop, 2 = one-shot), bits 12-13 encoding (1 = PCM16),
bit 14 enable. Bit 15 has no libctru equivalent because the service sets it.

The period reload is the catch. The register counts **up** to overflow, as on
the NDS sound hardware this descends from, so it takes the negative of the
wanted period:

```c
uint32_t timer  = 0x3FEC3FC / rate;          /* libctru's CSND_TIMER */
uint32_t reload = (0x10000u - timer) & 0xFFFFu;
MMIO16(CSND_CH(n) + 0x02) = (uint16_t)reload;   /* before the start bit */
MMIO16(CSND_CH(n) + 0x00) = CH_START | CH_ENABLE | fmt | loopmode;
```

Write the reload **before** setting the start bit, or the channel begins on
whatever the previous sound left behind.

## Why a read-back check is not proof

`+0x02` is write-only and reads as zero. `+0x08` accepts a divider and reads it
back perfectly, and the channel ignores it completely.

So "the value I wrote is in the register" says nothing about whether the channel
uses that register, and chasing the read-back leads directly away from the
answer. The layout above was settled by **timing playback**, not by reading
registers back.

## Finding it again

The harness that worked this out lived in a Sound Test screen, removed once
audio was working. It is worth knowing how it went, because the same approach
applies to any register whose effect cannot be read back:

* Play a tone of an exactly known length and time it against the MCU real-time
  clock. An ARM9 hardware timer does the counting, calibrated against one RTC
  second first, so the figure depends on no clock constant in this tree
  (`src/os/Timer9.c` still provides this). A ratio other than 1.0 is the factor
  by which the sample clock differs from what the divider assumes.
* Sweep every unclaimed offset in the channel block, writing the divider plain
  and two's-complement, timing each. The offset that yields the expected length
  is the register. That is what identified `+0x02` two's-complement.

A symptom worth recognising: playback at a period of exactly 65536 (about 7.8x
too slow for 8 kHz) means the reload register is reading zero, so the write is
not reaching the register the channel reads.

## The crash beep

A fault on either CPU plays three short beeps. The pattern is rendered once at
boot into `AUDIO_ERR_ADDR`, so playing it costs nothing but starting a channel.

That matters for the ARM11 path. CSND is a DMA engine, so a channel started
before a core stops still plays to completion; the ARM11 fault stub in
`audio11_start.s` therefore starts the tone with register stores alone, needing
no stack and no call, neither of which is safe in a faulted context. Those
stores hard-code the `AUDIO_ERR_*` values from `include/audio.h`, so the two
must change together.

The ARM9 path simply posts `AUDIO_CMD_ERROR` from `crash_handle()` before it
draws anything. When the ARM11 is what died that post is never serviced, which
is harmless because that core has already started the same sound itself.

Note that the ARM11 half only fires once the ARM11 exception vectors are
installed. `crash11_init()` in `Core11.c` is currently disabled, so an ARM11
fault is not caught at all today: no beep and no crash screen.

## Audio files

`.aaf` is a 16-byte header (`AAF1`, version, channels, bit depth, reserved,
sample rate, sample count) followed by raw mono PCM. `audio/aaf_tool.py`
converts and plays them on a PC; `load_track()` in `src/os/os_main.c` reads them
from `SD:\Aurora\Music`.
