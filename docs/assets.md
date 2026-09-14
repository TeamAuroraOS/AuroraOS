# Assets

Aurora's icons, wallpaper and type come from `SD:\Aurora\assets.pak`, built by
`tools/mkassets.py` from the art in `icons/` and the fonts in `assets/fonts/`.

The pack is optional. Without it the OS still boots and draws its own built-in
1bpp icons and 8x8 font; `assets_ok()` reports which is in use.

| Piece | File |
|-------|------|
| Pack builder | `tools/mkassets.py` |
| PNG reader | `tools/png_read.py` |
| TrueType reader and rasteriser | `tools/ttf.py` |
| Runtime loader | `src/assets.c`, `include/assets.h` |
| Generated ids | `include/asset_ids.h` |
| Drawing | `src/screen.c` (`draw_asset`, `draw_text`) |

## Why a pack, and not runtime scaling

The art is authored far larger than the console uses it: the icons are 80x80 and
the wallpaper is 2498x1778. Everything is resampled to the exact size the UI
draws it, once, at build time, with an area filter.

That is the whole trick behind the sharpness. Scaling on the console can only
ever be magnification of something already small, and magnifying a hard edge
spreads it across several pixels; that is blur, not detail. Resampling *down*
from the original art keeps detail that was there all along, and leaves the
console doing nothing but copying pixels it was handed.

It is also what makes it fast. A tinted coverage blit is a load, a test and (for
the minority of pixels that are not fully clear or fully solid) three
multiply-add-shifts. The bilinear path it replaced cost four filtered texture
fetches with bounds checks, three interpolations and a framebuffer offset
recomputed per pixel, and produced a softer result for the trouble.

## What the art actually is

All 28 UI icons are **white with an 8-bit alpha channel**: anti-aliased coverage
masks, not pictures. So they are stored as coverage alone, one byte per pixel,
and tinted when drawn, which is why a single copy serves every accent colour.

`background.png` is the same thing at a different scale, white at alpha 5..30, a
faint pattern of rounded cards meant to sit over a coloured ground. It is drawn
tinted with the accent colour, which is what gives the home screen its texture.

Four pieces are genuinely full-colour and are stored as RGBA and drawn
untinted: `files`, `preferences`, `store` and `game card`. The last one is easy
to get wrong, because its alpha channel is a solid card silhouette; reduced to
coverage it flattens to a plain white block, and only its 30 grey tones carry
the artwork. If a new icon comes out as a featureless shape, that is why.

`diolog-box.png` is *not* in the pack. It is a plain rounded rectangle, black at
alpha 191 with a radius of about 40 at 1200x232, so it is drawn with
`draw_gradient_round_rect` instead: identical output, no storage, and correct at
any size. `ui_dialog()` in `src/ui.c` is that panel.

## Sizes

Masks are stored at 64, 48, 32, 24 and 16 pixels: 24 is the File Explorer's list
rows and 16 the status bar. The long side is fitted so non-square art keeps its
aspect. The colour art is stored only at the sizes listed beside it in `COLOR`.
Fonts are baked at four sizes.

The app tiles (`files.png`, `preferences.png`, `store.png`) are square in the
source art, so `COLOR` marks them `rounded` and the builder cuts their corners to
a curve 22% of the icon's size, sampled 4x4 per pixel. Drawn square, they
clashed with the rounded cards around them. The game card is left as drawn.

The text-file icon has no PNG of its own: `text_page()` takes
`Unkonwn File.png`, clears its question mark and rules four lines into the page,
and the result is resampled like any other mask.

Nothing scales a size that is not in the pack, so add the size to `ICON_SIZES`
(or to the `COLOR` entry) rather than scaling at the call site.

## Pack format

Little-endian throughout; `src/assets.c` maps these directly onto structs, and a
compile-time check there fails the build if the compiler pads them.

```
PakHead   32 bytes  magic "APAK", version, count, data_off, arena, 3 reserved
PakEntry  24 bytes  id, type, w, h, pad, off, csize, usize    (count of them)
payloads            byte-RLE, one run per entry at its own `off`
```

`type` is 0 for ALPHA8 (`w*h` coverage bytes), 1 for RGBA (`w*h*4`), 2 for a
font. A font payload is:

```
AssetFontHead  16 bytes  first, count, ascent, descent, line_height,
                         atlas_w, atlas_h, pad
AssetGlyph     10 bytes  left, top, w, h, adv, pad, ax, ay   (count of them)
atlas                    atlas_w * atlas_h coverage bytes
```

`left`/`top` place the glyph against the pen with `top` counted upward, so
`draw_text` takes a baseline rather than a box top.

The RLE is one byte of control: `0x01..0x7F` is a run of that many copies of the
next byte, `0x81..0xFF` is that many literal bytes. It saves about 65%. Storing
coverage at 4bpp instead saved only another 2KB across the whole pack and would
have flattened the wallpaper's alpha-5 pattern to nothing, so the pack stays at
8bpp.

## On the console

`assets_load()` mounts the card, validates the header, decodes every entry into
a fixed arena and unmounts. Loading is all-or-nothing: any inconsistency leaves
`assets_ok()` false and the UI on its built-in art, so a stale or truncated pack
degrades rather than corrupts.

The arena is at `0x23F40000`, in the 768KB gap between the bottom backbuffer
(which ends by `0x23F38400`) and the app-launch staging area at `0x24000000`.
The current pack decodes to 529,248 bytes. `ASSET_ARENA_NEEDED` is emitted into
`asset_ids.h` and checked against `ASSETS_ARENA_MAX` at compile time, so a pack
that outgrew the gap fails the build rather than the console.

Because the pack lives on the card, the payload only grew by about 5KB.

## Adding or changing art

1. Put the PNG in `icons/`. A UI icon should be white with an alpha channel;
   anything else is stored as RGBA.
2. Add it to `MASKS` or `COLOR` in `tools/mkassets.py` with an id.
3. `make assets`. This rewrites `Aurora/assets.pak` and regenerates
   `include/asset_ids.h`.
4. Rebuild the OS, and copy the new pack to the card. The pack and the binary
   are checked against each other at load time, so both must be updated.

The builder asserts that every payload survives an RLE round-trip, so a pack
that writes successfully decodes correctly.

## Fonts and licensing

The UI type is **Figtree**, SIL Open Font License 1.1, Copyright 2022 The
Figtree Project Authors. The licence is `assets/fonts/OFL.txt`, and the TTFs it
covers are `assets/fonts/Figtree-{Regular,Bold}.ttf`. The copyright line
declares no Reserved Font Name.

Each atlas covers U+0020 to U+00FF: printable ASCII plus Latin-1, which holds
every accented letter Spanish and French use (acute, grave, circumflex,
diaeresis, tilde, cedilla, and the inverted marks). U+007F to U+009F are
control codes and get empty glyphs, as does any codepoint the font lacks.
Strings are UTF-8, decoded by `draw_text` and `text_width` in `src/screen.c`.
C sources write the accents as escapes such as `\u00E9`, so the tree
itself stays ASCII.

The glyph atlases in `assets.pak` are rendered from those fonts and so are
derived from them; they are covered by the same OFL 1.1, which is why the
licence text ships in the tree. The OFL is compatible with Aurora's GPL-3.0 for
distribution; it is recorded here and in the README rather than relicensed.

`tools/ttf.py` is Aurora's own code, not derived from any font engine: it reads
`cmap` format 4, `glyf`/`loca` (simple and composite) and `hmtx`, flattens the
quadratic outlines and fills them with the nonzero winding rule, sampling five
sub-scanlines per row and measuring exact horizontal span overlap for coverage.
