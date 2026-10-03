# Software 3D

| Piece | File |
|-------|------|
| Renderer | `src/os/Soft3D.c`, `include/soft3d.h` |
| Where it is used | `src/os/Model3D.c`; see [`glb.md`](glb.md) |
| Circle pad | `src/os/Touch11.c`, `src/os/Touch9.c`; see [`input.md`](input.md) |
| The GPU renderer and the 3D screen | see [`stereo3d.md`](stereo3d.md) |

The 3D Model screen draws with the PICA200's 3D pipeline. When the GPU is not
running, or a command list fails, it falls back to this renderer on the ARM9,
which draws a cube with six coloured faces, lit from above and in front, over
the same backdrop, with the same camera, orbit and stereo geometry. The bottom
screen then says `Software cube:` and why. The screen itself, its controls and
the GLB models are in [`glb.md`](glb.md).

The software renderer was confirmed on hardware, drawing the cube that was
then the whole screen.

## A frame

1. Wait for the previous present. It ends at a vertical blank, so the time
   after it is what counts as drawing.
2. Flush the D-cache by index (`os_dcache_flush`), then a GPU copy puts the
   backdrop into the top backbuffer. The backdrop is composed once each time
   the screen opens, at `0x26D60000` (288,000 bytes, clear of the animation
   frames that end at `0x26D58400`). Composing it per frame on the CPU would
   cost about 11 ms at the store rate measured in `gpu.md`; the copy takes
   about 1 ms.
3. `s3d_draw()`: build the camera from yaw and pitch, project the vertices,
   drop faces turned away, sort the rest far to near, shade each one flat and
   fill it, then smooth its edges.
4. Present the top screen. Every half second the bottom readout is redrawn and
   both screens go to the GPU in one operation (`anim_present(ANIM_BOTH)`), so
   the top does not lose a blank to the bottom's.

## The renderer

Everything is fixed point, because the ARM9 has no FPU and no divider:

| Quantity | Format |
|----------|--------|
| Positions, camera distance | `S3D_ONE` (4096) per unit |
| Angles | 65,536 per turn |
| Sines, cosines, unit vectors | 1/16384 |
| Screen coordinates | 16.16 pixels |
| Light levels | 1/256 (ambient 112, diffuse up to 160) |

`s3d_sin` folds the angle onto a quarter turn and evaluates the Taylor series
to x^9 in Q28; sin^2 + cos^2 stays within 0.02% of one. Divisions happen per
vertex and per edge, never per pixel.

A mesh (`S3dMesh`) is a vertex list and a face list. A face has three or four
vertices, in one plane, counter-clockwise seen from outside, and one colour.
Up to 64 of each.

### Visibility without a depth buffer

Faces are culled by their winding on screen, then drawn far to near by mean
depth (the painter's algorithm). For a convex mesh such as the cube that is
exact, and it needs no depth buffer: clearing one would mean 192 KB of CPU
stores per frame, about 7 ms at the store rate in [`gpu.md`](gpu.md) "The cost
of a CPU pixel", several times the cost of the whole cube. A mesh whose faces
cut through each other, or overlap in depth in ways a sort cannot order, will
need one; a GPU fill could clear it.

Faces with a vertex closer than a quarter unit are dropped rather than
clipped. The cube's camera never gets that close.

### Filling by columns

The framebuffer runs down each column (see [`gpu.md`](gpu.md) "The rendering
path"), so a face is filled one column at a time: each column is a single run
of bytes, written in words with the three-word BGR pattern, the way
`clear_screen` does.

A convex polygon's outline splits into the edges heading right and the edges
heading left, and each column takes its top and bottom from one of each. A
pixel is filled when its centre is inside: columns `ceil(a - 0.5)` up to but not
including `ceil(b - 0.5)` for an edge from `a` to `b`, and the same rule for
rows. Every edge is walked from its left end whichever face it belongs to, so
two faces on either side of an edge compute the same position in every column:
no gaps and no pixel drawn twice.

### Smooth edges

The fill rule alone leaves stair steps. After a face is filled, each of its
edges blends the face colour into the row of pixels just outside, by how much
of each pixel the face covers: half at the edge, nothing a pixel away, measured
square to the edge. That touches a few hundred pixels per frame.

An edge shared with a face drawn later is left alone: the later face smooths it
and blends straight into the earlier one. Smoothing both sides would let the
background show through the seam.

## Tuning the circle pad

`CPAD_DEAD` (150) and `CPAD_FULL` (1000) in `Model3D.c` are the dead zone and
full deflection in raw ADC units. Orbiting works with them on a New 3DS, but
the codec's real range has not been measured: the rest point should read
about 2048 on both axes, and a full push shows how far it really goes.

If an axis turns the wrong way, flip it in `cpad_read()` in `Touch9.c`, which
follows GodMode9's convention (the ADC's X runs right to left).
