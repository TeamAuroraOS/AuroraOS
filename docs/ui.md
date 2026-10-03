# Drawing and animation

How the UI draws its shapes (`src/screen.c`) and how it animates
(`src/os/Anim.c`, `include/anim.h`). The rendering path underneath, backbuffers
and GPU presents, is in [`gpu.md`](gpu.md).

## Rounded rectangles

`draw_filled_round_rect` and `draw_gradient_round_rect` share one renderer,
`round_rect()`. It walks the shape a framebuffer column at a time, so every
pixel is written once, and it builds the column's shades once from a single
ramp, so the body, the sides and the corners all take the same colour at the
same height.

Each corner arc is centred `r` pixels in from both edges, measured between
pixels rather than through pixel centres. The outermost pixel of an arc, next to
a straight side, is then fully covered, and the curve runs into the side without
a step. A corner pixel's coverage is the distance from its centre to the arc,
which needs a square root; it depends only on the radius, so `round_tab()`
works it out once per radius and keeps twelve radii (up to 40).

`draw_round_ring` draws the outline of the same shape, `t` pixels thick. Its
inner edge is the shape inset by `t` with radius `r - t`, so a corner pixel's
coverage is the outer shape's less the inner one's, and whatever is inside shows
through.

### What this replaced

The old version drew a gradient rect as three gradient strips plus four flat
corners, and that is where the visible faults came from:

* The side strips ran the whole ramp over `h - 2r` rows while the middle ran it
  over `h`, so the sides were lighter than the middle at the top and darker at
  the bottom: thin bars down both sides of every card and tile.
* Each corner was one flat shade taken from the ramp at the corner's height,
  which did not match the darker strip ending right above it. The bottom
  corners came out lighter than the sides they joined, which read as small
  white edges on the large icon tiles.
* The arcs were centred through pixel centres, so the corner pixel beside a
  straight side was only half covered: a notch where the curve met the side.
* An unselected tile or row was erased by filling its ring area with one flat
  colour, an estimate of the background at that height. Over the wallpaper's
  gradient and pattern that left a faint square around every tile.

Anything redrawn in place is now erased with `ui_wallpaper_rect`, which copies
the exact background, or the whole screen is repainted. A flat-colour patch is
only right on a screen whose background is that colour, as in the setup wizard.

## Animation

Everything runs on the ARM9 timer, so an animation lasts the same time whatever
the frame rate, and frames are capped at one every 16 ms. Transitions and
cross-fades need the GPU backbuffers; without them a screen simply appears,
while the eased movements still run. Only a timer that does not tick at all
turns animation off.

**Settings > GPU Test** shows the state on the top screen: whether animation is
on, the timer rate and whether it came from the RTC, and the last transition's
kind, frame count and length. That is the first thing to read if nothing moves.

### Screen transitions

```
anim_transition(ANIM_PUSH, ANIM_BOTH);   /* before drawing the next screen */
...draw it and present, as usual...
```

`anim_transition()` copies what the panels show into a spare frame, then holds
the presents of the screens it names (through the `g_screen_intercept` hook in
`screen.c`). Once each of them has presented, the animation runs, from the old
frame to the new. If a screen never presents, `ui_idle()` runs the transition
with whatever did, so a screen that redraws one panel still animates. A screen
not named presents as usual.

| Kind | Look | Time |
|------|------|------|
| `ANIM_PUSH` | the new screen slides in from the right over the old one, which drifts left at 30% of the speed under the new screen's shadow | 360 ms, ease out |
| `ANIM_POP` | the old screen slides away to the right, uncovering the one under it, which drifts back into place | 360 ms, ease out |
| `ANIM_FADE` | cross-fade | 220 ms, ease out |
| `ANIM_POPUP` | a dialog card grows from 80% to full size with a small overshoot while it fades in, and the screen behind it dims | 300 ms |

A slide frame is built in the backbuffer from two bands of columns, one from
each frame. The framebuffer runs down screen columns, so a band of columns is
one block of memory and each band is one GPU copy; the ARM9 only darkens the
20 columns of shadow. A fade and a pop-up blend on the ARM9, a word (two
channels per multiply) at a time; the pop-up scales the card by sampling it from
the new frame. If the GPU refuses a copy, the ARM9 does it instead.

The convention: a screen arms `ANIM_PUSH` as it opens and `ANIM_POP` as it
returns, and the screen it returns to animates when it redraws.
`open_screen()` in `os_main.c` does this for the screens it opens; the File
Explorer, the terminal, the text viewer and the hex editor do it themselves.
Dialogs pop up and arm a fade for whatever the caller draws next, so they fade
out:

```
anim_popup(x, y, w, h);      /* the card's rectangle */
...draw what goes behind the card, such as the scrim...
anim_popup_behind();
...draw the card, and present...
```

`ui_dialog()` does this itself on the bottom screen, so every message pops up;
`fv_confirm()`, the File Explorer menu and its progress card do it too.

| Where | Kind |
|-------|------|
| Home Menu to Settings, Music, Files, 3D Model, the terminal, and back | slide |
| Home Menu into a folder, and out | slide |
| Settings to each of its pages, About to More Info, and back | slide |
| File Explorer into a folder, and up | slide |
| File Explorer to the text viewer, hex editor or keyboard, and back | slide |
| Setup wizard pages, and its keyboard | slide |
| Messages, questions, the File Explorer menu and progress card, launch messages | pop-up in, fade out |
| Image viewer (top screen), touch calibration targets | fade |
| Applying an accent colour, which re-tints the wallpaper | fade |
| Start-up, and the end of the setup wizard | fade in from what the boot left |
| Power Off from the Home Menu's bar | fade to black |

### Cross-fades in place

The `anim_xfade_*` calls fade parts of one screen to a new state without
holding the loop up, the new content sliding 28 pixels in from one side as the
old leaves the other way, or fading in place:

```
anim_xfade_begin(ANIM_TOP);              /* keep what the screen shows now */
...draw the new state...
anim_xfade_area(ANIM_TOP, x, y, w, h);   /* up to four areas */
anim_xfade_start(ANIM_TOP, dir);         /* 1 from the right, -1 from the left, 0 */
screen_present_top();
```

and then, on each due frame, `if (anim_xfade_frame(ANIM_TOP))
screen_present_top();`. A new change part way through starts from what is on
screen, so holding the D-pad never jumps. It lasts 200 ms. The Home Menu's app
preview and the accent picker's swatch slide in the direction the selection
moved; the File Explorer's icon and card and the music player's track name,
which follow vertical lists, fade in place.

### Springs: selections, tiles and lists

An `AnimVal` is a position on a damped spring, stepped in 2 ms slices: it moves
off quickly, settles gently, and stays smooth when the target moves again part
way. The plain spring is critically damped; one with `bounce` set overshoots by
about 5% before it settles, which is what makes a selection feel springy. An
`AnimList` pairs two of them for a list, the selected row (bouncy) and the
scroll offset (plain). `anim_step` asks for a redraw only when the pixel a value
rounds to changes.

The selection ring is drawn with `draw_round_ring` over the tiles or rows, one
pixel thicker than the gap around them so that it also covers their softened
edge. It springs between tiles on the Home Menu and between swatches in the
accent picker; in Settings, the File Explorer and the music player it springs
between rows while the list scrolls to keep it in view. Rows scrolling past the
ends of the File Explorer and music lists go under the bars above and below
them.

On the Home Menu the selected tile also lifts, growing 3 pixels on each side,
and dips under a press for about 110 ms before its screen opens. Every tile's
position is a spring too, across all the pages laid side by side, with one
more spring scrolling the strip: turning a page, tiles moving aside for a
carried one and a dropped tile settling all come from those. See
[`home.md`](home.md).

A screen that animates repaints all of itself each frame rather than patching:
the wallpaper comes back as one GPU copy, then the rows or tiles and the ring
are drawn over it. A loop drives it like this:

```
int ms = anim_frame(&frame_at);          /* 0 until a frame is due */
if (ms && anim_list_step(&list, ms))
  paint_the_list();                      /* and present */
```

### Memory

The frames the animations work from sit clear of the screenshot buffers and the
ARM11 mailbox:

| Address | Size | Use |
|---------|------|-----|
| `0x26C00000` | 288,000 B | top screen: the old frame |
| `0x26C50000` | 288,000 B | top screen: the new frame |
| `0x26CA0000` | 230,400 B | bottom screen: the old frame |
| `0x26CE0000` | 230,400 B | bottom screen: the new frame |
| `0x26D20000` | 230,400 B, to `0x26D58400` | bottom screen: what is behind a pop-up card |

The in-place cross-fades use the same frames; a transition cancels any
cross-fade still running.

### Limits

* Each panel is triple buffered and switches frames at its vertical blank,
  found by whichever method the console turned out to support at boot (see
  [`gpu.md`](gpu.md) "Presenting without tearing"); GPU Test shows what each
  panel uses and how many presents found a blank. That also paces animations
  to the display, and `anim_present()` sends both screens in one operation so
  a two-screen animation still gets a frame per refresh.
* Transitions hold the loop for their length, so a press that starts and ends
  inside one is not seen.
* A fade or pop-up blends every byte it covers on the ARM9. One screen keeps
  up well; a fade of both screens at once runs at a lower frame rate. Slides
  cost the ARM9 almost nothing.
