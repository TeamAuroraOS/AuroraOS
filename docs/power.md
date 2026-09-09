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

The home menu's top bar (`hm_status_bar()` in `src/os/os_main.c`) shows the
clock, the date, the charge percentage, and a battery pill whose fill is
proportional to charge. The fill is green while charging, red at 15% or below,
and the accent colour otherwise. The setup wizard's status bar shows the clock
and date through the same calls.

The clock is kept live from the home-menu loop: the MCU is sampled periodically
rather than every frame (it is a slow bus), and the bar is only repainted when
the displayed minute actually changes, which, with the GPU doing the present,
costs one blit a minute.

## Known caveat: RTC offset

Aurora reads the hardware RTC directly. The stock 3DS system software stores a
*offset* in its configuration and displays `RTC + offset`, so Aurora's clock can
disagree with what System Settings shows even though the hardware register is
being read correctly. Reading the raw RTC is the right foundation; applying or
editing an offset would be a separate feature.
