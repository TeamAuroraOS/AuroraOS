# Assets

Aurora's icons, wallpaper, type and its own few sounds come from
`SD:\Aurora\assets.pak`, built by `tools/mkassets.py` from the art in `icons/`,
the fonts in `assets/fonts/` and the WAV files in `assets/audio/`.

The pack is optional. Without it the OS still boots and draws its own built-in
1bpp icons and 8x8 font, silently; `assets_ok()` reports which is in use.

| Piece | File |
|-------|------|
| Pack builder | `tools/mkassets.py` |
| PNG reader | `tools/png_read.py` |
| TrueType reader and rasteriser | `tools/ttf.py` |
| Resampler, DSP-ADPCM encoder | `tools/dspadpcm.py` |
| Runtime loader | `src/assets.c`, `include/assets.h` |
| Generated ids | `include/asset_ids.h` |
| Drawing | `src/screen.c` (`draw_asset`, `draw_text`) |
| Playing the sounds | `src/os/UiSound.c` (see [`audio.md`](audio.md)) |

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

All 32 UI icons are **white with an 8-bit alpha channel**: anti-aliased coverage
masks, not pictures. So they are stored as coverage alone, one byte per pixel,
and tinted when drawn, which is why a single copy serves every accent colour.

`background.png` is the same thing at a different scale, white at alpha 5..30, a
faint pattern of rounded cards meant to sit over a coloured ground. It is drawn
tinted with the accent colour, which is what gives the home screen its texture.

Five pieces are genuinely full-colour and are stored as RGBA and drawn
untinted: `files`, `preferences`, `store`, `game card` and the auroraShop logo
(`store-logo.png`, 191x152, cut from the aShop mock-up; it is not square, so it
is in its own list, `ART`, at its own size). The last one is easy
to get wrong, because its alpha channel is a solid card silhouette; reduced to
coverage it flattens to a plain white block, and only its 30 grey tones carry
the artwork. If a new icon comes out as a featureless shape, that is why.

The seven faces in `icons/people/` are the credits on the About page's More Info
screen. They are full-colour too, listed in `PEOPLE` rather than `COLOR` because
the builder cuts them to a circle (`round_corners` at half the size) instead of
to `ROUND_FRAC`.

`diolog-box.png` is *not* in the pack. It is a plain rounded rectangle, black at
alpha 191 with a radius of about 40 at 1200x232, so it is drawn with
`draw_gradient_round_rect` instead: identical output, no storage, and correct at
any size. `ui_dialog()` in `src/ui.c` is that panel.

## Sizes

Masks are stored at 64, 48, 32, 24 and 16 pixels: 24 is the File Explorer's list
rows, and 16 the status bar, the Home Menu's bar and the small icons inside a
folder's tile (for which `files`, `store` and `game card` are also stored at 16). The long side is fitted so non-square art keeps its
aspect. The colour art is stored only at the sizes listed beside it in `COLOR`.
Fonts are baked at four sizes.

The app tiles (`files.png`, `preferences.png`, `store.png`) are square in the
source art, so `COLOR` marks them `rounded` and the builder cuts their corners to
a curve 22% of the icon's size, sampled 4x4 per pixel. Drawn square, they
clashed with the rounded cards around them. The game card is left as drawn.

`Folder.png` was drawn for the Home Menu's folders in the same stroke style as
the rest (80x80, a 7-pixel line); `folder-icon-without-text.png`, a filled
colour folder, is not in the pack.

`Search.png` and `Wrench.png` were drawn for aShop's buttons in the same
style (80x80, a 7-pixel rounded stroke). aShop's banners and welcome art are
not in the pack: they are content, read from `SD:/Aurora/Store` when the store
opens (see `docs/store.md`), and the pack's arena has little room left.

The text-file icon has no PNG of its own: `text_page()` takes
`Unkonwn File.png`, clears its question mark and rules four lines into the page,
and the result is resampled like any other mask.

Nothing scales a size that is not in the pack, so add the size to `ICON_SIZES`
(or to the `COLOR` entry) rather than scaling at the call site.

## Pack format

Little-endian throughout; `src/assets.c` maps these directly onto structs, and a
compile-time check there fails the build if the compiler pads them.

```
PakHead   32 bytes  magic "APAK", version, count, data_off, arena,
                    sound_off, sound_count, 1 reserved
PakEntry  24 bytes  id, type, w, h, pad, off, csize, usize    (count of them)
payloads            byte-RLE, one run per entry at its own `off`
PakSound  24 bytes  id, format, rate, samples, off, size  (sound_count, at sound_off)
sounds              one payload per sound at its own `off`
```

The sound table and its payloads come after every image payload. A pack from
before them has `sound_off` and `sound_count` zero, and simply has no sounds.

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

## Sounds

| Id | Source | When it plays |
|----|--------|---------------|
| `ASSET_SOUND_STARTUP` | `startup.wav` | once, when the Home Menu appears after a cold boot |
| `ASSET_SOUND_SHOP` | `Aurora-SHOP.wav` | in aShop, looped |
| `ASSET_SOUND_DL_DONE` | `DL-complete.wav` | in aShop, when an install finishes |
| `ASSET_SOUND_SELECT` | `select.wav` | A or a tap opens or does something in the Home Menu, Settings or aShop |
| `ASSET_SOUND_BACK` | `back.wav` | B or a Back button leaves a folder, screen or pop-up there |
| `ASSET_SOUND_MOVE` | `move.wav` | the selection moves there |
| `ASSET_SOUND_HOME` | `Aurora-HOME.wav` | while the Home Menu is on screen, looped, at 70% |

`mkassets.py` reads WAV only, so a song that arrives as `.m4a` is converted
first: `ffmpeg -i Aurora-HOME.m4a -map 0:a:0 -map_metadata -1 -bitexact -c:a
pcm_s16le Aurora-HOME.wav` (the `-map` leaves out the cover art). ffmpeg honours
the file's gapless information, so the WAV is exactly the 1,732,810 frames the
m4a says it holds. Only the WAV goes in the repository: `.gitignore` keeps
`assets/audio/*.m4a` out, as nothing builds from them.

Exactly where the last three play is in [`audio.md`](audio.md), "Menu sounds".

The sources are 44.1 or 48 kHz stereo. The builder stores only what the
console plays:

- **Mono.** A voice sends one channel to both speakers, and the song's two
  channels are 97% the same anyway.
- **32,728.5 Hz**, the CSND mixer's rate. The core turns the stored rate,
  32728, into the divider `0x3FEC3FC / 32728 = 2048`, which is exactly that
  rate, so a channel passes the samples through unchanged. `resample()` in
  `tools/dspadpcm.py` is a 64-tap Blackman-windowed sinc cut off at 14.7 kHz.
  The song has 0.3% of its energy above 11 kHz.
- **DSP-ADPCM**, the 4-bit format of the GameCube and 3DS DSPs. For each sound
  the builder designs 8 predictor pairs: (0, 0) plus a k-means of the frames'
  least-squares predictors. Then for every frame of 14 samples it picks the
  pair and shift with the least error.

`SOUNDS` in `tools/mkassets.py` also says which digital silence (exact zeros)
to cut: none, the start (`TRIM_LEAD`) or both ends (`TRIM_BOTH`). Both songs are
17 ms of silence and then whole bars at 110 BPM (8 for aShop's, 18 for the Home
Menu's), so looped as they were, every pass would land 17 ms late. DL-complete
had 0.35 s of silence before its chime.
The menu sounds come as 3.2 s files, of which select and back sound for 0.66 s
and move for 0.1 s; the rest is cut.

| Sound | Source | Stored | Bytes | SNR |
|-------|--------|--------|-------|-----|
| startup | 2.595 s, 498,276 bytes | 84,917 samples, 2.595 s | 48,560 | 65 dB |
| shop | 17.475 s, 3,082,666 bytes | 571,360 samples, 17.458 s | 326,528 | 37.6 dB |
| DL_DONE | 1.853 s, 355,876 bytes | 49,279 samples, 1.506 s | 28,192 | 66 dB |
| select | 3.200 s, 614,444 bytes | 21,457 samples, 0.656 s | 12,296 | 66.9 dB |
| back | 3.200 s, 614,444 bytes | 21,456 samples, 0.656 s | 12,296 | 69.6 dB |
| move | 3.200 s, 614,444 bytes | 3,270 samples, 0.100 s | 1,904 | 46.2 dB |
| home | 39.293 s, 6,931,284 bytes | 1,285,431 samples, 39.276 s | 734,568 | 42.8 dB |

Move's lower figure still puts the error near -71 dBFS, since the sound itself
peaks at -25 dBFS. The three menu sounds added 26 KB to the pack, and the Home
Menu's song 735 KB (1,516,068 bytes now).

The SNR compares the decoded sound with the mono 32.7 kHz signal, so it counts
only the ADPCM's loss. On the song, IMA-ADPCM (the format CSND decodes
itself) reached 21 dB at the same size, which is audible grit. 8-bit mu-law
needs 1.75 times the space for about the same 37 dB. The builder is plain Python
(the Makefile's interpreter has no numpy), so `make assets` takes about a
minute, most of it the song.

A sound payload is 16 little-endian int16 coefficients (8 pairs, Q11), then
8-byte frames. A frame header is `pair << 4 | shift`, followed by 14 signed
4-bit residuals, high nibble first. Each sample is

```
s = (n * (2048 << shift) + 1024 + c1 * h1 + c2 * h2) >> 11, clamped to 16 bits
```

where `h1` and `h2` are the two samples decoded before it, both 0 at the
start. The builder keeps every coefficient below 2.0 (4075), and the loader
refuses one beyond 8.0, a pair above 7 or a shift above 12. Together those keep
the sum inside 32 bits.

## On the console

`assets_load()` mounts the card, validates the header, decodes every entry into
a fixed arena and unmounts. Loading is all-or-nothing: any inconsistency leaves
`assets_ok()` false and the UI on its built-in art, so a stale or truncated pack
degrades rather than corrupts.

The checks (`assets_read()`): the index starts right after the header; each
entry has a known id seen only once, sizes that are not zero, and a payload
inside the file; each RLE stream decodes to exactly its size; an image is
`w*h` bytes (`w*h*4` for RGBA), and a font's head, glyphs and atlas add up; the
entries fill exactly the arena size the header names; and all four fonts are
present. A card that does not answer at once gets three tries. When the pack
still fails, the OS says so once at boot ("assets.pak not loaded / Using
built-in art. Press A.") and carries on with the 8x8 font and 1-bit icons.

Sounds are not decoded into the arena (it has 55 KB left), and they do
not take part in the all-or-nothing rule. `assets_read()` only reads their
table, and an inconsistent table drops every sound while the art stays. A
valid entry has an id seen once, format 0, a rate from 1000 to 96000, a size
that matches its sample count, and a payload inside the file. Ids this build
does not know, from a newer pack, are skipped. `asset_sound_read(id, dst, max,
&rate)` decodes one sound into the caller's buffer through the loader's
scratch buffer, on a card the caller has mounted. It returns 0 for a sound
longer than `max` samples, and for a payload that does not read back or
decode. For a long sound, `asset_sound_open(id, buf, buf_bytes, dst, max, &s,
&rate)` reads it packed into `buf`, after which the card is not needed, and
`asset_sound_decode(&s, frames)` decodes it a slice at a time; `s.done ==
s.total` when it is finished.

Older loaders read nothing past the images, so the OS and apps built before the
sounds still load this pack. HEAD's `assets.c` loaded it in a PC test, as did
the new loader with the pack from before the sounds and with damaged sound
tables (its art loaded every time). That test also showed `asset_sound_read`
gives bit-identical samples to `decode()` in `tools/dspadpcm.py`, for all six
sounds once the menu sounds were added. On 2026-11-01 a PC run of the real
`assets.c` showed the same for the Home Menu's song, through both
`asset_sound_read()` and slices of `asset_sound_decode()` of varying size; a
damaged frame stops the stream, and a buffer too small is refused.

The arena is at `0x23F40000`, in the 768KB gap between the bottom backbuffer
(which ends by `0x23F38400`) and the app-launch staging area at `0x24000000`.
The current pack (185 assets) decodes to 730,368 bytes, 93% of the gap. A pack with a different count is refused, so a C app built against an older one falls back to the built-in font until it is rebuilt (`gfx_fonts_loaded()`); it still runs. `ASSET_ARENA_NEEDED` is emitted into
`asset_ids.h` and checked against `ASSETS_ARENA_MAX` at compile time, so a pack
that outgrew the gap fails the build rather than the console.

Because the pack lives on the card, the payload only grew by about 5KB.

## Adding or changing art

1. Put the PNG in `icons/`. A UI icon should be white with an alpha channel;
   anything else is stored as RGBA.
2. Add it to `MASKS`, `COLOR` or `PEOPLE` in `tools/mkassets.py` with an id.
3. `make assets`. This rewrites `Aurora/assets.pak` and regenerates
   `include/asset_ids.h`.
4. Rebuild the OS, and copy the new pack to the card. The pack and the binary
   are checked against each other at load time, so both must be updated. An
   added asset moves the ids after it (Beta 7's team avatar moved the
   wallpaper and the fonts), and an older pack is then refused because its
   fonts are no longer where the OS looks.

The builder asserts that every payload survives an RLE round-trip, so a pack
that writes successfully decodes correctly.

A sound: put the WAV (8- or 16-bit PCM, any rate, mono or stereo) in
`assets/audio/` and add it to `SOUNDS`. `make assets` gives it an
`ASSET_SOUND_*` id, and `-v` prints each sound's size and SNR. A sound id does
not move any art id, so a pack with new sounds still suits an older OS. The
OS's buffers hold 4 s for each short sound, 4 s for the three menu sounds
together and 28 s for the music ([`audio.md`](audio.md)); a longer sound is
refused, not cut.

## Themes

The wallpaper is not drawn from the pack each frame. `ui_bg_build()` in
`src/ui.c` composes it once per screen (the gradient with the pack's pattern
tinted by the accent colour) into a cache at `0x25010000` (top, 288,000
bytes) and `0x25060000` (bottom, 230,400 bytes), and every wallpaper paint is
a copy from there. A theme from Settings > Theme (PR 24) fills that cache
with pictures from the card instead: `ui_theme_load(top, bottom)` decodes
`SD:/Aurora/themes/<Name>/top.png` and `bottom.png` with `image_load()`
(through the image buffers at `0x25100000` and `0x26100000`), requires
exactly 400x240 and 320x240, and writes them in the framebuffer's rotated
B,G,R layout. While a theme is in use `ui_bg_build()` leaves the cache alone,
so an accent change does not touch it; `ui_theme_clear()` goes back to the
composed wallpaper. Settings checks a theme from the PNG headers alone
(`image_probe()`), so only `ui_theme_load()` decodes. If a picture fails to
decode after the top one was already copied in, `ui_theme_load()` drops the
theme, and `theme_select()` in `src/os/os_main.c` loads the theme that was
showing before, or leaves the built-in wallpaper to be composed at the next
paint. The pack itself is unchanged by a theme: icons and fonts still come
from it. The user side is in [`home.md`](home.md), "Wallpaper
themes".

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
