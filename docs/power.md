# Clock and battery (MCU)

The console's management MCU carries the real-time clock and the battery gauge.
Aurora reads both over I2C, device 3 (bus 1, address `0x4A`), through
`src/power.c` / `include/power.h`.

Call `I2C_init()` once before using anything here. `os_main()` does this during
startup, before the first status bar is drawn.

## Registers

| Register | Meaning | Confidence |
|----------|---------|------------|
| `0x0B` | Battery charge percentage, 0-100 | well established (`MCUHWC_GetBatteryLevel` reads this) |
| `0x0F` | Power / charger flags | register is right; **the charging bit is not**, see below |
| `0x30`..`0x36` | RTC: sec, min, hour, weekday, day, month, year, all BCD | confirmed on hardware |

The RTC layout is not a guess: the crash handler's power-off countdown already
depends on `0x30` counting seconds, and that works on a real console.

Year is stored as an offset from 2000. Some fields carry flag bits above their
BCD digits (the hour register can hold a 12/24-hour selector), so `rtc_read()`
masks each field before decoding: `0x3F` for hours and day, `0x1F` for month,
`0x07` for weekday, `0x7F` for seconds and minutes.

`rtc_read()` also range-checks the decoded result and returns 0 if it cannot be
a real date. A dead or half-initialised MCU tends to return all `0x00` or all
`0xFF`, and it is much better to show `--:--` than a convincing but wrong clock.

## The charging bit is unverified

`MCU_STATUS_CHARGING` in `src/power.c` is currently `1 << 4` of register `0x0F`.
That the register holds the power flags is documented; *which* bit means "charger
attached" is not, in any source I could confirm.

It is deliberately isolated as a single `#define` so it is a one-line change.

**To verify:** watch the battery pill in the top-right of the home menu. It turns
green while charging. Plug the charger in and out; if the colour does not
follow, the bit is wrong. `power_status_raw()` returns the whole register if you
want to compare its value with and without the charger attached; the bit that
differs is the right one.

Everything else about the battery is unaffected if this bit is wrong: the
percentage, the fill width and the low warning do not depend on it.

## What the UI shows

The top status bar (`status_bar_draw()` in `src/os/StatusBar.c`, shared by the
Home Menu and the setup wizard) shows the clock, the date, the model tag, the
Wi-Fi and grid indicators, the charge percentage, and a battery pill whose fill
is proportional to charge. The fill is green while charging, red at 15% or
below, and the accent colour otherwise. Without the asset pack it falls back to
the 8x8 font and drawn indicators.

The clock is kept live from the home-menu loop: the MCU is sampled periodically
rather than every frame (it is a slow bus), and the bar is only repainted when
the displayed minute actually changes, which, with the GPU doing the present,
costs one blit a minute.

## Console model

Which console Aurora is running on comes from `CFG11_SOCINFO` (`0x10140FFC`).
Bit 0 is set on every retail unit; **bit 1 is set on the New 3DS family**, which
is the bit that matters. `src/model.c` / `include/model.h` wrap it, and the home
menu's status bar shows an **N** on a New model and nothing on an Old one.

That register is ARM11 config space, so the ARM9 cannot read it directly. The
ARM11 core samples it once at start-up into `AudioCtrl.socinfo` and the ARM9
reads it from there. Reading it on the ARM9 would be a single instruction, but
that side has already been seen to data-abort on ARM11 peripheral registers, and
a fault during boot is a far worse failure than a missing indicator.

Two consequences follow from where it comes from:

* The model is only known once `audio_boot()` has brought the ARM11 up. That
  happens before anything is drawn, so the status bar always has it.
* If that core never starts, `socinfo` stays zero and the model reads as Old.
  That is the safe default, because it only suppresses an indicator.

**Not verified on hardware.** The bit assignment is the one bare-metal 3DS
projects use, but it has not been checked on a real New 3DS here. If the letter
is wrong, `aurora_socinfo()` returns the raw register so the actual value can be
read out.

## Known caveat: RTC offset

Aurora reads the hardware RTC directly. The stock 3DS system software stores a
*offset* in its configuration and displays `RTC + offset`, so Aurora's clock can
disagree with what System Settings shows even though the hardware register is
being read correctly. Reading the raw RTC is the right foundation; applying or
editing an offset would be a separate feature.
