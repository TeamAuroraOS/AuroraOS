# The camera

| Piece | File |
|-------|------|
| The Camera app (capture and gallery) | `src/os/Camera.c`, `camera_app_run()` in `include/camera.h` |
| The driver on the ARM11 core | `src/os/Cam11.c` (`cam11_command`, `cam11_tick`) |
| The shared block and buffers | `include/camera.h` |
| The command | `AUDIO_CMD_CAM` (16) in `include/audio.h`, handled in `src/os/Core11.c` |
| The camera for C apps | `sdk/runtime/app_cam.c`, "Camera" in `sdk/include/aurora_app.h` |
| The permission for apps | Settings > Camera, `USER_DAT_CAMERA` (byte 54) in `include/user.h` |
| Icons | `icons/camera.png` (`ASSET_APP_CAMERA_64/32/16`), `icon_camera_bits` in `include/icons.h` |

**Status:** new in Beta 8 v0.1.7-everest (PR 51 by MooniiCodes, merged
2026-11-23). It builds; no console result is recorded here.

The driver is a port of the camera code in Octoblimp's
[Android Eclair 3DS](https://github.com/Octoblimp/Android-Eclair-3DS)
(`drivers/platform/nintendo3ds/ctr_cam.c` and
`camera/libcameraservice/CameraHardwareN3ds.cpp`). `src/os/Cam11.c` carries
an `SPDX-License-Identifier: GPL-2.0` line; the README credits the project
and ships its Apache-2.0 licence as `LICENSE-CAMDRIVER`.

## The Camera app

The camera icon in the Home Menu's top bar opens it (`home_camera()` in
`src/os/os_main.c`). It starts on the back (outer) camera and remembers the
last camera used while the OS runs.

| Screen | Shows |
|--------|-------|
| Top | the live picture, 320 x 240 in the middle, with 40-pixel bars in the theme's colour at each side; "Starting Back Camera..." until the first frame |
| Bottom | a card with "Back Camera" or "Front Camera" and a dot (green once frames come, amber before), the Shutter button, Flip (X) and Gallery (Y) with the number of photos |

| Input | Does |
|-------|------|
| R, or tap Shutter | saves the picture and flashes the top screen white for six frames |
| X, or tap Flip | switches between the back and front cameras |
| Y, or tap Gallery | opens the gallery on the newest photo |
| B | stops the camera and goes back to the Home Menu |

A photo is saved as `SD:/Aurora/Photos/IMG_NNNN.bmp`: the raw camera frame
(no bars or status bar), 320 x 240, 24-bit, bottom-up, 230,454 bytes. The
number starts at the number of photos plus one and skips names that exist.
The bottom card says "Saved IMG_0001.bmp", or "Save failed (check SD)", for
90 passes of the loop.

### The gallery

The gallery lists the `.bmp`, `.png` and `.jpg` files in
`SD:/Aurora/Photos` (up to 64, names up to 31 characters), in directory
order.

| Input | Does |
|-------|------|
| Left / Right, or tap Prev / Next | the previous or next photo, with a cross-fade |
| X, or tap Delete | asks "Delete this photo?", then deletes it |
| B, or tap Camera | back to the camera |

A 24-bit BMP 320 or 400 pixels wide and 240 tall is drawn straight from the
file; anything else goes through `image_load()` and is scaled to fit.

### The camera light

The app turns the camera LED on when it starts a camera and off when it
stops (MCU register 0x2B: 2 on, 0 off). The OS also writes 0 at every start,
so the light is off on the Home Menu after an app that left it on.

## The driver

The ARM9 cannot reach the camera, so the core runs it (`src/os/Cam11.c`,
from core 150). The OS sends `AUDIO_CMD_CAM` with `arg0` = `CAM_OP_START` (1) or
`CAM_OP_STOP` (0) and `arg1` = `CAM_FRONT` (0) or `CAM_BACK` (1).
`CAM_OP_CAPTURE` (2) is defined but nothing handles it: a photo is a copy of
the newest frame. `cam_cmd()` in `Camera.c` waits for the GPU to be idle,
posts the command and waits for the ack.

Both cameras are Aptina MT9V113 sensors (chip version 0x2280) on I2C bus 0
(`0x10161000`): the back camera at 0x78, the front one at 0x7A (8-bit
addresses). The core drives that bus with its own time-bounded routines.

**Start** (`cam_start_hw()`):

1. turns on the camera clock (`PDN_CAM_CNT`, `0x10141224`, bit 0) and puts
   the other sensor in standby;
2. reads the chip version; no answer, 0 or 0xFFFF gives `CAM_ST_NO_SIGNAL`;
3. resets the CAM0 receiver (`0x10120000`) and sets the sensor up: reset,
   wake, YUV 4:2:2, the PLL (with the post-divider `p1`), context A at
   320 x 240 and context B at 640 x 480, read mode 0x0025 for both sensors,
   auto-exposure index up to 12, then the refresh commands;
4. starts the receiver, trimmed to 320 x 240, and sets `CAM_ST_READY`.

**Frames** (`cam11_tick()`, every pass of the core's loop): the core drains
the receiver's FIFO (`0x10320000`, a 0x80-byte window) for at most 1.5 ms at
a time into the YUYV buffer. A frame starts after a gap of at least 3 ms
with an empty FIFO (the sensor's blanking); if no gap shows up within two
frames' worth of data, the stream is taken as gapless and framed by size.
When 38,400 words are in, `cam_deliver_frame()` converts them to BGR24 in the
screens' column-major layout (JFIF full-range YCbCr) in the buffer not being
shown, switches `ready_buf`, adds one to `frame_seq` and sets
`CAM_ST_LIVE`. The ARM9 copies that buffer straight into a framebuffer.

| Setting in `CamShared` | Value at start |
|------------------------|----------------|
| `yc_mode` | 2: decide YUYV or UYVY from every frame (luma varies far more than chroma), so dark first frames cannot lock in the wrong order |
| `flip_v` | 1 |
| `flip_h` | 1 for the back camera, 0 for the front one (read mode 0x0025 already mirrors it) |
| `swap_uv` | 1 |

On a FIFO overrun the core restarts the receiver and counts it in
`overruns`; after three in a row it raises `p1` by one (up to 7), which
slows the sensor's pixel clock, and sets the sensor up again. The value it
reaches is used for every later start until the core is replaced.

**Stop**: stops the receiver, puts both sensors in standby (when one
answered), turns the clock off and sets `CAM_ST_OFF`.

### `CamShared` (`0x233AC000`)

| Field | Meaning |
|-------|---------|
| `status` | `CAM_ST_OFF` 0, `READY` 1, `LIVE` 2, `NO_SIGNAL` 3 |
| `active_cam`, `chip_id`, `width`, `height` | as named |
| `frame_seq`, `ready_buf` | a new frame, and which BGR buffer holds it |
| `luma_odd` | the byte order the last frame used (0 YUYV, 1 UYVY) |
| `words_read`, `overruns`, `p1` | diagnostics |
| `flip_v`, `flip_h`, `swap_uv`, `yc_mode` | the conversion, above |
| `app_allowed` | Settings > Camera, for C apps; written by the OS at boot and on every change |

### Memory

| Address | Size | Use |
|---------|------|-----|
| `0x233AC000` | 64 B | `CamShared` |
| `0x26B20000` | 153,600 B | the frame as it comes in, YUYV |
| `0x26B48000` | 230,400 B | BGR frame 0 |
| `0x26B84000` | 230,400 B | BGR frame 1 (to `0x26BBC400`) |

The buffers sit in the gap between the screenshot buffers (which end at
`0x26B1CA36`) and the animation frames (from `0x26C00000`), which nothing else
uses in the OS or in apps; a static assert in `camera.h` keeps them inside
it. PR 51 had put them at `0x26E40000`, inside aShop's arena, which was safe
only while the camera and aShop never ran together; they moved on 2026-11-26
(core 151).

## The camera in C apps

```c
#include <aurora_app.h>

int main(void) {
  if (!cam_start(CAMERA_BACK))
    return 0;                    /* off in Settings: the user saw a pop-up */
  while (app_loop()) {
    if (hid_keys_down() & KEY_B)
      break;
    if (cam_ready())
      cam_draw(SCREEN_TOP, 40);  /* 320 wide, centred on the top screen */
  }                              /* app_loop() presents the frame */
  return 0;                      /* cam_stop() runs on the way out */
}
```

| Function | Does |
|----------|------|
| `cam_allowed()` | true when Settings > Camera is on |
| `cam_start(CAMERA_FRONT or CAMERA_BACK)` | turns the LED on and starts streaming; when the setting is off it shows a "Camera Off" pop-up ("please turn it on in settings") until A, B, START or a tap on OK, and returns false |
| `cam_stop()` | stops streaming and turns the LED off; also called when the app leaves |
| `cam_ready()` | true once a frame is in |
| `cam_frame_seq()` | goes up by one per new frame |
| `cam_frame_bgr()` | the newest 320 x 240 frame, column-major BGR24, or NULL |
| `cam_draw(screen, x_off)` | copies the frame into a screen at column `x_off` (0 to 80 on the top screen, 0 on the bottom one) |
| `cam_save_bmp(path)` | writes the frame as a 24-bit BMP |
| `cam_set_led(on)` | the LED; turning it on needs the setting |

**Settings > Camera** is off by default and applies only to C apps; the
built-in Camera app ignores it. The runtime checks `app_allowed` in
`CamShared`, then byte 54 of `USER.dat`. The setting is new in this release,
so it needs the C SDK 1.7 runtime (Beta 8): rebuild an app to use the camera.
There is no camera example in `sdk/examples` yet.
