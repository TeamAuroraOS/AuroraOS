# Touch calibration and screenshots

| Piece | File |
|-------|------|
| Touch reading, on the ARM11 | `src/os/Touch11.c` |
| Readings to pixels, calibration values | `src/os/Touch9.c`, `include/touch.h` |
| Calibration screen | `src/os/TouchCal.c`, `include/touchcal.h` |
| Screenshots | `src/os/Screenshot.c`, `include/screenshot.h` |

## Touchscreen

The touch ADC sits inside the CTR audio codec, so the ARM11 core reads it over
the codec's SPI bus and publishes raw 12-bit readings in `TouchShared` at
`0x233A0000`. The register sequence and data layout were worked out from
GodMode9's codec driver (GPL-2.0-or-later); see the header of `Touch11.c`.

The ARM9 turns a reading into a pixel with one straight line per axis:

```c
x = (raw_x - x_min) * 320 / (x_max - x_min);
y = (raw_y - y_min) * 240 / (y_max - y_min);
```

`x_min` and `x_max` are the readings at the left and right edges, and `y_min`
and `y_max` at the top and bottom, held in a `TouchCal`. A reversed pair makes
that axis run the other way. Until a calibration is saved, the first-guess
values in `Touch9.c` are used.

## Calibration

**Settings > Touch Calibration** shows four targets, 40 pixels in from each
corner, one at a time, clockwise from the top left. The row in Settings reads
Default or Custom.

* Each target takes the average of one press. The first two readings of a press
  are skipped while it settles, at most 64 are used, and a press shorter than
  four readings counts as a slip. A press already down when a target appears
  has to lift first, so one long press cannot answer two targets.
* The two targets on each edge are averaged, and a line per axis through those
  averages gives the readings at the screen's edges.
* If the two readings for one edge differ by more than a quarter of the span,
  a tap missed its target and the screen asks for the four again.
  `touch_cal_set()` also refuses a span under 256, about a fourteenth of a real
  one.
* A check screen follows, where drawing leaves dots under the stylus, with faint
  crosses at the targets and the centre to aim at. **A** keeps the calibration,
  **X** measures again and **B** puts the old one back.

A kept calibration is in use at once and saved in `SD:\Aurora\USER.dat`. From
offset 40 the record holds a flag byte and then `x_min`, `x_max`, `y_min` and
`y_max` as little-endian 16-bit values. That is `USER_DAT_VERSION` 2; a version
1 file has zeros there and reads as no calibration. At boot a saved calibration
that `touch_cal_set()` refuses is ignored.

## Screenshots

Pressing **L** and **R** together on any OS screen saves both screens as one
400x480 BMP, the top screen above the bottom one, which is centred with black
either side. The check lives in `get_keys_down()`, which every screen polls, and
L and R are then left out of what it reports, so the screen does not act on
them too. Auric apps read the buttons themselves, so this does not work inside
an app.

1. Any GPU present still in flight is collected, so the panels hold one whole
   frame.
2. Both panels are copied out of VRAM to `0x26A00000` and `0x26A50000`, clear
   of the image viewer's buffers and below the ARM11 mailbox. The picture is the
   frame that was showing, however long the card write takes.
3. The BMP is built at `0x26A90000`. The framebuffers already store a pixel as
   B, G, R, the order BMP wants, and 400 times 3 is a multiple of 4, so rows
   need no padding.
4. It is written to `SD:\Aurora\Screenshots\`, named from the clock as
   `2026-09-16_14-05-33.bmp`, with `_2` and up for a second shot in the same
   second, or `Screenshot_001.bmp` and up when the clock cannot be read.
5. "Screenshot saved", or "Screenshot not saved", is drawn straight onto the top
   panel for 0.9 seconds and then painted out from the copy, so the running
   screen's own frame is never touched.

The card may already be mounted by the screen that is running, such as the File
Explorer. Mounting over that would invalidate its open files, so the screenshot
checks with `f_opendir("0:/")` and mounts the card itself only when nothing has.
