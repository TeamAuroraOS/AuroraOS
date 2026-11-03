# Clock, battery and power (MCU)

The console's management MCU carries the real-time clock, the battery gauge and
the power switch. Aurora talks to it over I2C, device 3 (bus 1, address
`0x4A`), through `src/power.c` / `include/power.h`.

Call `I2C_init()` once before using anything here. `os_main()` does this during
startup, before the first status bar is drawn.

## Registers

| Register | Meaning | Confidence |
|----------|---------|------------|
| `0x0A` | Battery temperature, degrees C (`battery_temperature()`); the only temperature the console reports | 3dbrew I2C registers |
| `0x0B` | Battery charge percentage, 0-100 | well established (`MCUHWC_GetBatteryLevel` reads this) |
| `0x0C` | The percentage's fraction, /256 (`battery_tenths()` adds it) | 3dbrew I2C registers |
| `0x0D` | System voltage on the load side, raw x 5/256 V (`battery_millivolts()`) | 3dbrew I2C registers |
| `0x0F` | Power / charger flags | register is right; **the charging bit is not**, see below |
| `0x30`..`0x36` | RTC: sec, min, hour, weekday, day, month, year, all BCD | confirmed on hardware |
| `0x20` | Power control: bit 0 powers off, bit 2 reboots | bits from GodMode9; power-off confirmed on hardware |
| `0x22` | LCD power: bit 0 turns both screens off | used before every power-off, as GodMode9 does |

The RTC layout is not a guess: the crash handler's power-off countdown already
depends on `0x30` counting seconds, and that works on a real console.

Year is stored as an offset from 2000. Some fields carry flag bits above their
BCD digits (the hour register can hold a 12/24-hour selector), so `rtc_read()`
masks each field before decoding: `0x3F` for hours and day, `0x1F` for month,
`0x07` for weekday, `0x7F` for seconds and minutes.

`rtc_read()` also range-checks the decoded result and returns 0 if it cannot be
a real date. A dead or half-initialised MCU tends to return all `0x00` or all
`0xFF`, and it is much better to show `--:--` than a convincing but wrong clock.

## The charging bit

`MCU_STATUS_CHARGING` in `src/power.c` is currently `1 << 4` of register `0x0F`.
That the register holds the power flags is documented; *which* bit means "charger
attached" is not, in any source I could confirm.

It is deliberately isolated as a single `#define` so it is a one-line change.
On a New 3DS the pill follows the charger (2026-10-04).

**To verify:** watch the battery pill in the top-right of the home menu. It turns
green while charging. Plug the charger in and out; if the colour does not
follow, the bit is wrong. `power_status_raw()` returns the whole register if you
want to compare its value with and without the charger attached; the bit that
differs is the right one.

Everything else about the battery is unaffected if this bit is wrong: the
percentage, the fill width and the low warning do not depend on it.

## Power off and reboot

`power_shutdown()` and `power_reboot()` turn the screens off through `0x22`,
drain the ARM9 write buffer, then write bit 0 or bit 2 of `0x20` and wait for
the power to go. The screens go off first because the MCU can hang otherwise.
Neither function returns.

The Home Menu's power icon (after asking), its old-core prompt and the system
menu's Power Off (L + Down + Select, [`home.md`](home.md)) use
`power_shutdown()`,
and the terminal's `shutdown`, `poweroff`, `reboot` and `systemctl
poweroff|reboot` use both. The crash handler keeps its own copy of the
power-off sequence in `src/os/crash.c`, so a fault never depends on more code
than it has to.

Inside an app the system menu runs on the ARM11 core, so from core 118
`src/power.c` and `src/i2c.c` are compiled a second time for the core
(`build/os_power11.o`, `build/os_i2c11.o`) and the core calls the same
`power_shutdown()`. The drain instruction is a data synchronisation barrier on
the ARM11. A change to either file changes the core, so it needs an
`AUDIO_CORE_VERSION` bump.

## Closing the lid

Closing the shell turns both backlights off and holds the OS where it reads
the buttons; opening it brings the light and the input back. It is a pause,
not the console's sleep mode: the LCDs stay powered, both CPUs keep running
(the ARM9 in a busy-wait loop), sound keeps playing (except the Home Menu's
song, which stops and restarts when the lid opens), the Wi-Fi chip is left
as it is, and the framebuffers are kept as they were. Added by PR 29
(2026-10-24, core 120); its author reports that it works on a console, and no
other result is recorded here.

The lid sensor is GPIO1 bit 2 (`0x10147000`, 3dbrew's "Shell closed"),
which reads 1 while the shell is closed. Both CPUs read it:

* **The ARM11 core** (`core11_sleep_poll()` in `src/os/Core11.c`) checks it
  every 32,768 passes of its command loop, about every 100-150 ms depending
  on the clock, and on every button read of the in-app system menu. On a
  close it remembers both panels' PWM duty and writes 0. On an open it puts
  back the duties of the brightness level in use (`backlight11_refresh()`),
  or the remembered ones if no level was ever sent. While closed,
  `backlight11_refresh()` changes nothing, so a brightness command or a 2D/3D
  switch is applied at the next open. The core goes on taking commands. It
  does not look at the lid while it runs a long command (a Wi-Fi join, an
  HTTP exchange, an `AUDIO_CMD_RUN11` job), so the light goes off when that
  command ends.
* **The ARM9** (`include/lid.h`): `aurora_lid_wait_open()` spins in steps
  of 60,000 `nop`s until the lid opens. `get_keys()` in
  `src/os/os_main.c` calls it, so every OS screen stops at its next button
  read; so do long jobs whose cancel check reads the buttons (a file copy,
  an aShop download between pieces, a terminal command). Whichever
  `get_keys()` call waited sets a flag that the next `get_keys_down()` acts
  on: it returns nothing and takes the buttons still held as already
  pressed, so the hand that opens the console cannot press A by accident.
  The OS's animations cap each frame's step, so they carry on smoothly.
* **Apps** get the same wait from their runtime. The C SDK (1.3) waits in
  `pad()` (`sdk/runtime/app_hid.c`, under every `hid_` read and so every
  `app_loop()`) and in `app_wait_ticks()` (`sdk/runtime/app_sys.c`, behind
  `sys_sleep()`, `sleep()`, `usleep()` and `nanosleep()`). The Auric runtime
  waits in `aur_check_home()`, which every built-in calls, and in its button
  read. Both stop the app's clock meanwhile: `app_ticks64()` (so
  `sys_millis()`, `sys_micros()`, `clock()`, the frame pacing, `app_fps()`
  and the SDK's waits) and Auric's `millis()` leave the closed time out, so
  a game resumes where it was instead of seeing one huge frame. The SDK's
  wait keeps reading the timer, so it sees the 32-bit count wrap however
  long the lid stays shut. Wall-clock time (`time()`, `gettimeofday()`,
  the RTC) is not affected. Buttons held as the lid opens are not new
  presses in either runtime. An app built before this change keeps running
  behind the dark screens; so does code that never reads the buttons or
  waits through the runtime. Every app in this tree (`output/apps`,
  `Games/Tetris.BIN`) was rebuilt with it on 2026-10-24.

The firm's launcher does not watch the lid.

## Screen brightness

Each panel's LCD block (`0x10202200` top, `0x10202A00` bottom; 3dbrew "LCD
Registers") holds the backlight: `+0x40` is the PWM duty (bits 0-9, 0 turns
the light off) and `+0x44` the period minus one (bits 0-9) with bit 16 the
enable; boot code and libn3ds set `0x1023E`, a period of 575. Bit 0 of `+0x00`
turns on ABL, the adaptive backlight, which moves the duty with the picture.
The ARM9 data-aborts on that block, so the ARM11 core does the work:
`audio_set_brightness(level)` (`src/os/Audio9.c`) waits for a pending GPU
present, then posts `AUDIO_CMD_BRIGHTNESS` (14) with a level from 0 to 4.

From core 119, `backlight11_set()` in `src/os/Core11.c`:

1. turns ABL off on both panels, as libn3ds does "for steady PWM output",
   since a set level would not hold with it on;
2. enables the PWM with the usual period if bit 16 is clear;
3. writes the duty for that level and panel, scaled to the period found.

The duties come from libn3ds's default calibration (launch-day Old 3DS): the
3DS's five steps are 20, 43, 73, 95 and 117 cd/m2, and a quadratic per panel
turns them into PWM.

| Level | Top (2D) | Top (3D) | Bottom |
|-------|----------|----------|--------|
| 1 / 5 | 15 | 36 | 32 |
| 2 / 5 | 33 | 80 | 71 |
| 3 / 5 | 57 | 140 | 123 |
| 4 / 5 | 75 | 187 | 162 |
| 5 / 5 | 94 | 237 | 203 |

The top panel needs less drive than the bottom one in 2D and more behind the
parallax barrier, so `stereo11_mode()` calls `backlight11_refresh()` when the
top screen goes in or out of 3D. Core 117 and 118 wrote 20, 43, 73, 95 or 117
(the luminance itself) to both panels and left ABL as it was. After every
change the core copies the top flags, the top duty, the top PWM register and
the bottom duty into `AudioCtrl.diag4` to `diag7`.

**Settings > Brightness**: A opens the screen. Five bars on the bottom screen
and "N / 5" on the top; Left/Right (also Up/Down or the circle pad), or a tap
on a bar, changes the level at once. A saves it in `SD:\Aurora\USER.dat` at
offset 51, B puts the old level back. The top screen's last line shows the
duties the core read back ("Backlight PWM 75 / 162", plus "ABL on" if the bit
did not clear), which tells a level that did not reach the panel from one
that did. Left/Right on the Settings row itself also steps the level and
saves it. At boot `os_main()` sends the saved level, or level 3 ("4 / 5")
for a file without one. The byte is read only from
`USER_DAT_VERSION` 3 files: older ones have a zero there, which would be the
darkest level. The launcher's own Settings
(`set_brightness()` in `src/main.c`) still only stores the value, since the
firm's ARM9 has no core to ask.

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
Text names the family: "New 3DS/2DS" or "Old 3DS/2DS" (About, `systemctl`,
the clock-switch messages), since the bit cannot tell a 2DS from a 3DS.

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

## The About page

**Settings > About** shows the version, the model, the battery, the free space
on the card and the licence. The free space starts as "..." and is filled in
when it is known: on a card whose FAT32 information sector holds no valid
count, FatFs has to read the whole allocation table, which used to freeze the page
for seconds on a large card. Since PR 21 the page keeps the card mounted
while it is open and calls `f_getfree_step()` (added to `src/ff.c` and
`include/ff.h`, beside FatFs's own `f_getfree()`) once per loop pass, for at
most four FAT sectors, so B and More Info answer at once; the count is cached
in the mounted volume when it completes. Its **More Info** button opens a page laid out like
the stock System Settings, with the version, a software-update line that says
there is none, the eMMC CID and the device; the bottom screen credits the team
and LeweckBanana, who made the Home Menu's song, with their faces from the
asset pack (`PEOPLE` in `tools/mkassets.py`).

Two of those rows are limited by what a bare-metal ARM9 can see:

* **The printed serial number cannot be read.** It lives in `SecureInfo_A` in
  CTRNAND, which is encrypted with per-console keys Aurora does not have, so the
  row shows the **eMMC CID** instead: the internal storage's own identification,
  which is readable and unique to the console.
* **Device** is New or Old only. The XL and 2DS variants are recorded in the
  same encrypted NAND, while `CFG11_SOCINFO` only separates the families.

`sdmmc_nand_cid()` in `src/sdmmc.c` reads the CID. The eMMC is device 1 on the
controller the SD card uses, and nothing else in Aurora touches it, so the read
takes it through identification only (CMD0, CMD1 until it reports ready, then
CMD2 for the CID) and leaves it there. The controller is pointed back at the SD
card afterwards, and every SD transfer retargets it anyway, so a card read is
unaffected. The CID is kept after the first read, which costs a few
milliseconds. The bytes are shown in the order GodMode9 prints them, so the two
can be compared directly.

## Clock offset

Aurora reads the hardware RTC and never writes it: the only MCU registers it
writes are `0x20` and `0x22`, to power off. The 3DS's own software never sets
the RTC from System Settings either. It keeps an *offset* in its encrypted
configuration and shows `RTC + offset`, so the raw RTC can be hours away from
the time the 3DS menu shows. An RTC that runs on UTC, for example, reads four
hours ahead of US Eastern Daylight Time.

Aurora cannot read that offset, so it keeps its own. **Settings > Clock** shows
the time with Hour and Minute boxes: Up and Down change the focused box, Left
and Right move between them, X goes back to the raw RTC, A keeps the change and
B drops it. The change shows in the status bar at once. It is stored in
`SD:\Aurora\USER.dat` at offset 49 as minutes (little-endian int16, zero in
older files), wraps to between 12 hours behind and 14 ahead, and is applied
inside `rtc_read()` (`g_rtc_offset`, carried through the day, month and year),
so the status bar, the terminal's `date` and screenshot names all agree. The
RTC itself, and with it the 3DS's own clock, is left as it was.
