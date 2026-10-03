# GLB models and the 3D Model screen

| Piece | File |
|-------|------|
| The 3D Model screen | `src/os/Model3D.c`, `include/model3d.h` |
| GLB loader | `src/os/Glb.c`, `include/glb.h` |
| JSON tokenizer | `src/os/Json.c`, `include/json.h` |
| Texture decoding | `src/image.c` (PNG), `src/jpeg.c` (JPEG), `include/image.h` |
| Vertex shader | `src/os/model.v.pica`, built into `build/model_shbin.h` |
| Command lists, stereo | `src/os/P3d9.c`; see [`stereo3d.md`](stereo3d.md) |
| Software fallback | `src/os/Soft3D.c`; see [`soft3d.md`](soft3d.md) |

**Status: working on a New 3DS** (2026-10-02, with a textured model). Other
models' files were checked on the PC; see "What has been checked".

## Using it

Copy a binary glTF file to **`SD:/Aurora/model.glb`**, then open **Home Menu >
3D Model**. The bottom screen shows the load (Reading, Parsing, Meshes,
Textures), then the model appears on the top screen, centred and scaled to a
radius of 2 units, seen from 4.5 units away.

| Control | Does |
|---------|------|
| Circle Pad | orbits round the point looked at, the way the pad is pushed, up to 120 degrees a second; pitch stops at 85 degrees either way |
| D-pad | pans that point across the screen, at 60% of the viewing distance a second; it stays within 6 units of the model's centre |
| Y | zooms in: the distance shrinks by a steady fraction a second, down to 0.3 units |
| A | zooms out, up to 40 units |
| 3D slider | stereoscopic 3D, deeper as it rises |
| X | New 3DS, in 3D: the next barrier position |
| B | back to the Home Menu |

The bottom screen, redrawn twice a second:

| Line | Meaning |
|------|---------|
| `NN.N fps` | frames presented in the last half second |
| `NN.N ms a frame, drawing N.N ms` | frame interval, and the time to draw both eyes |
| `Title: N triangles, N textures` | `asset.extras.title` from the file, or `model.glb` |
| a note, when there is one | textures that could not be decoded, or why the file was refused (then `: a cube`) |
| `Closest: name` | the part nearest the viewer (see below) |
| `New 3DS, 3D slider N% (raw N), 3D on` | the console, the slider, 2D or 3D |
| `Barrier: ...` | New 3DS in 3D: the barrier position X selects |

Without a card, or when the file is refused, the screen shows a built-in
coloured cube through the same GPU path. If the GPU is not running, or fails
mid-way, it falls back to the software renderer's cube and says why.

## The closest part and the 3D

Every frame, `closest_vertex()` finds the vertex nearest the camera, measured
along the view direction, among those inside the view. It works on a
fixed-point copy of the positions (1/4096 unit), and samples at most 32,768
vertices a frame (every nth vertex of a larger model), so its cost stays a few
milliseconds whatever the model.

Its depth becomes the stereo **convergence distance**: that point lands on the
same pixel for both eyes, so the closest part sits exactly on the screen's
surface and everything else goes into the screen. Nothing ever comes out of the
screen, which keeps the 3D comfortable as the camera orbits and zooms, and
avoids parts cut off by the screen's edges while floating in front of it. The
eye separation is 3.5% of that distance at the slider's top, so a point at
infinity separates by 8.4 pixels at most (`EYE_FRAC` in `Model3D.c`). The part
the vertex belongs to (`glb_prim_of()`, a binary search of the primitives) is
shown as `Closest:`; its name is the node's, else the mesh's, else `Mesh N`.

## What the loader reads

| glTF feature | Support |
|--------------|---------|
| Container | `.glb` version 2, everything in the one file. A `.gltf` with separate `.bin` or image files is not read; convert it to `.glb` first |
| Scene | `scene`, or the first; the node tree with `matrix` or translation, rotation and scale; up to 512 mesh instances, 32 levels deep |
| Primitives | triangles, triangle strips and fans; points and lines are skipped and counted |
| `POSITION` | float, or any integer type with `KHR_mesh_quantization`, normalized or not |
| `NORMAL` | used for the lighting; when missing, made by averaging the faces round each vertex |
| `TEXCOORD_n` | the set the base colour texture names; float or normalized integers |
| `COLOR_0` | RGB or RGBA, multiplied into the base colour |
| Indices | 8, 16 or 32 bit, or none; an index past the end makes an empty triangle |
| Material | `baseColorFactor` and `baseColorTexture`; `alphaMode` OPAQUE, MASK (with `alphaCutoff`) and BLEND; `KHR_materials_unlit`; `KHR_texture_transform` on the base colour texture |
| Sampler | wrap REPEAT, CLAMP_TO_EDGE and MIRRORED_REPEAT; NEAREST magnification, otherwise linear; always mipmapped |
| Images | PNG (every bit depth and colour type, palette transparency; not interlaced) and baseline JPEG (grey or colour; not progressive), stored in the file |
| Ignored | metallic/roughness, normal, occlusion and emissive maps; skins, morph targets, animations, cameras, lights; sparse accessors |

A file whose `extensionsRequired` names anything other than the three
extensions above is refused with `needs extension NAME`, for instance
`KHR_draco_mesh_compression` or `EXT_meshopt_compression`. Most tools can
export without compression.

The lighting is baked into the vertex colours when the model loads: an ambient
term, a key light from above and in front, and a dimmer fill from the other
side, the same for every model. Unlit materials take the base colour as it is.

### Limits

| What | Limit | Beyond it |
|------|-------|-----------|
| File | 11 MB | refused |
| Primitives drawn | 512 | refused |
| Vertices in one primitive | 65,535 | refused (the GPU indices are 16-bit) |
| Materials | 256 | the rest drawn plain |
| Distinct textures | 64 | the rest drawn untextured |
| Texture size | 256 x 256 | scaled down |
| Everything converted | 16 MB | refused, `the model does not fit in memory` |

A model's vertices take 40 bytes each, plus 12 for the copy the closest-part
search reads, and 2 per index. A 256 x 256 texture with its mipmaps is about
350 KB, so the 16 MB holds about 45 of them with a modest mesh; textures that
no longer fit count as not decoded and their parts are drawn untextured.

### Textures

Each image is decoded straight to its texture size, never whole:

1. `image_probe()` reads the width and height from the header.
2. The texture is the largest power of two at or below each side, between 8
   and 256 (the PICA200 needs powers of two). A 2048 x 1024 image becomes
   256 x 256; a 300 x 200 one, 256 x 128.
3. `image_decode_fit()` box-filters the image down as it decodes. A PNG is
   inflated through a 32 KB window and handed over a row at a time, so its
   full size never has to fit anywhere. A JPEG is decoded whole when its RGB
   fits in the 8 MB image buffer, and otherwise from each 8x8 block's average
   alone (an eighth of each side: a 4096 x 4096 JPEG still gives 512 x 512).
   An image smaller than its texture (under 8 pixels, or a large JPEG's
   eighth) is scaled up by nearest neighbour.
4. Colour is averaged weighted by alpha plus one: clear pixels do not darken
   their neighbours, and a fully clear area keeps its colour, so the GPU's
   filtering leaves no dark fringe round cut-outs.
5. Mipmaps are halved the same way down to 8 pixels, and every level is
   swizzled into the PICA200's tiled RGBA8 order (8x8 tiles, Morton order
   inside, bytes A, B, G, R).

A material marked BLEND whose texture and vertex colours are all opaque is
drawn as OPAQUE: exporters mark many materials BLEND, and opaque parts write
depth and need no sorting.

## Drawing

The model is converted once into what the PICA200 reads directly: one vertex
buffer (position, texture coordinate, lit colour; 40 bytes a vertex), 16-bit
index lists, and textures. Each eye's frame is one command list:

1. The backdrop gradient, in screen space, depth test off.
2. Opaque parts, then MASK parts (alpha test at the cutoff), then BLEND parts
   sorted far to near by their centre, without writing depth.
3. Per material: texture unit 0 (address, size, mipmap levels, wrap,
   filters), combiner stage 0 (texture x vertex colour; alpha from the
   texture for MASK and BLEND, one for OPAQUE), alpha test and depth writes.
4. Per primitive: the buffer's offset to its first vertex, then
   `DrawElements` with its index list, following citro3d's sequence.

The vertex shader (`model.v.pica`) transforms the position by one 4x4 matrix
and copies the texture coordinate and colour.

## Memory

| Address | Size | Use |
|---------|------|-----|
| `0x24000000` | 16 MB | the converted model (the app-launch staging area, free while the OS runs) |
| `0x25100000` | 6 MB | while loading: generated normals, texture levels, decoder rows |
| `0x25700000`, `0x26100000` | 8 MB each | while loading: the image decoders' buffers |
| `0x26D60000` | 288,000 B | software fallback's backdrop |
| `0x26DB0000` | 288,000 B | right eye's frame, in 3D |
| `0x27100000` | 11 MB | `model.glb` as read from the card |
| `0x27C00000` | 1 MB | JSON tokens (52,428; a longer JSON chunk is refused) |
| `0x27D00000` | 2 x 256 KB | the two eyes' command lists |

The loading buffers belong to the image viewer and file tools, which cannot
run at the same time.

## What has been checked

On the PC, with the loader, the decoders and the screen's own code compiled
for x86:

* **A real model** (12,252 vertices, 16,802 triangles, 19 primitives, 17
  textures): loads with nothing skipped, and a reference renderer that samples
  the tiled textures the way the PICA200 addresses them draws it correctly
  textured and the right way up from four sides.
* **A test file covering the rest:** a 2048 x 1024 RGBA PNG, a 1600 x 1200
  JPEG with `KHR_texture_transform` and clamped edges, a 16-bit grey PNG, a
  palette PNG with transparency on an unlit MASK material, a 2 x 2 PNG, a grey
  JPEG, quantized positions and texture coordinates, a strip, a fan with 8-bit
  indices and 16-bit colours, nested nodes with a matrix, a point primitive,
  a progressive JPEG and an image in another file. Every texture decodes as
  expected; the last two fail alone and the parts draw in their base colour.
* **Refused files:** a required Draco extension, a file cut in half, a file
  that is not a GLB and an accessor running past its buffer each give their
  message, and the screen shows the cube.
* **The command list** `gpu_eye()` builds for the real model, decoded: buffer
  base `0x24000000 >> 3`, stride 40 with three attributes, each primitive's
  vertex offset, index offset and count, each texture's address, size and
  levels, and the opaque, then blended, state all match the loaded data.

On a New 3DS: the user's textured model displays correctly.

## On the console

1. Put a model at `SD:/Aurora/model.glb` and open **3D Model**. The loading
   card should step through Meshes and Textures, and the bottom screen should
   then name the model with its triangle and texture counts.
2. Orbit with the circle pad, pan with the D-pad, zoom with Y and A. Textures
   should sit the right way up, with no seams or speckles.
3. Watch `Closest:` change as different parts come nearest.
4. Raise the slider: the nearest part should stay at the screen's surface and
   the rest go behind it, at any zoom.

| Symptom | Likely cause |
|---------|--------------|
| The cube, with a reason in the note | the reason says what the loader refused |
| Model drawn, textures look scrambled | the tiling order in `tile()` (`Glb.c`) |
| Textures upside down | the `1 - v` flip in `build_prim()` |
| Parts flicker or vanish | the index or vertex offsets: compare the decoded list with the primitive table |
| Screen falls back to the software cube | the bottom line names where the GPU stopped |
| Large textures shimmer | mipmaps are not taking effect: the LOD register in `p3d_texture()` |

## Licence notes

`Glb.c`, `Json.c` and `Model3D.c` are Aurora's own code under GPL-3.0, written
from the glTF 2.0 specification and its extensions' published texts. The
texture tiling follows 3dbrew and tex3ds (zlib); the draw sequence follows
citro3d (zlib). No code was copied.
