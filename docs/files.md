# File Explorer, images and audio

The File Explorer browses the SD card, identifies what each file is, and opens
what Aurora can open. It is reached from the Files tile on the Home Menu.

| Piece | File |
|-------|------|
| Explorer screen and its menu | `src/os/Files.c`, `include/files.h` |
| Copy, move, delete, new folder | `src/os/FileOps.c`, `include/fileops.h` |
| On-screen keyboard | `src/os/os_setup.c`, `include/keyboard.h` |
| Text viewer, hex editor | `src/os/FileView.c`, `include/fileview.h` |
| BMP and PNG decoding, scaling | `src/image.c`, `include/image.h` |
| Baseline JPEG | `src/jpeg.c` |
| WAV reading and playback | `src/wavload.c`, `src/wav.c`, `include/wav.h` |
| Shared UI helpers | `src/ui.c`, `include/ui.h` |

## Browsing

FatFs is built with long file names (`FF_USE_LFN=1`), and its API takes and
returns them as UTF-8 (`FF_LFN_UNICODE=2`), the encoding the UI draws. A name
is up to 255 bytes; one that would not fit in that as UTF-8 comes back as its
8.3 alias instead, which still opens. A directory is read in one pass into a
fixed array of 256 entries, each holding a whole name, and sorted (folders
first, then by name, ignoring case); the list is never re-read while scrolling,
because `f_readdir` is slow enough that paging from the card would show.

A name too wide for where it is drawn is cut at a whole character and ended with
"...", by `ui_fit()` in `src/ui.c`. Only the drawing is cut; opening, renaming
and copying always use the full name. Paths are held in 1,024 bytes; a folder
too deep for that says so rather than opening.

D-pad moves, left and right jump a page, **A** opens, **B** goes up a level and
leaves the explorer at the root, **X** (or the Menu button) opens the file
menu, **START** exits. A tap selects a row, and a tap on the row that is
already selected opens it, so nothing launches from a single stray touch.

## File operations

The menu acts on the selected item, or on the folder when it is empty:

| Button | Does |
|--------|------|
| Copy, Move | marks the item; the top screen shows "Copying" or "Moving" and its name |
| Paste | copies or moves the marked item into the folder being shown |
| Rename | edits the name on the on-screen keyboard |
| Delete | asks first, then deletes the file, or the folder and everything in it |
| New folder | makes a folder, named on the keyboard |

* **Copy** walks a folder recursively and copies file data through the image
  viewer's file buffer at `0x25100000`, 1 MB at a time. Pasting into the folder
  the item came from names the copy "name (2).ext", then "(3)", and so on. A copy
  that fails or is cancelled deletes what it had made, so no half-copied file is
  left behind.
* **Move** is a FatFs rename, so it is instant whatever the size. Moving an item
  into a folder that already has that name is refused, and so is moving a folder
  into itself.
* **Delete** removes a folder's contents while reading it, as FatFs's own
  delete example does. A cancelled delete stops where it is.
* Copy and delete show a progress card, redrawn at most every 120 ms since each
  redraw repaints the list beneath it. **B** cancels.
* The keyboard adds `-`, `_` and `.` to the letters and digits it has in setup.
  **B** deletes, **L** toggles capitals, **START** or OK accepts and **SELECT**
  cancels. A name that is left empty changes nothing.
* A walk goes at most 24 folders deep and keeps one directory handle per level,
  so its memory is fixed however large the tree is.
* The terminal's `cp`, `mv` and `rm` use the same functions; see
  [`terminal.md`](terminal.md).
* Copies do not keep their dates. FatFs is built without a clock
  (`FF_FS_NORTC=1`), so every file it writes is dated 1 June 2025.

Errors come back as a `FoResult` and are shown in plain words by `fo_error()`:
a taken name, a full card, a read-only item, or an item that is no longer
there.

## Redrawing

A cursor move starts animations rather than repainting in place. The selection
ring glides to the new row, and the list scrolls when the window has to move;
each frame repaints the bottom screen from the cached wallpaper, one GPU copy,
then the rows and the ring. Rows scrolling past either end go under the path bar
and the footer, which are repainted over them. On the top screen only the icon
and the card change, and they cross-fade. Entering a folder slides the new
listing in from the right and going up slides it in from the left; dialogs and
the X menu fade in and out. The details are in [`ui.md`](ui.md).

The first version redrew everything with the CPU on every keypress, both
backgrounds included (a gradient pass plus a wallpaper blend pass over 400x240
and 320x240): about 497,000 pixels against the home menu's 41,000, and it
showed. With the wallpaper back as one GPU copy, a full repaint costs only the
rows.

Two things still matter for anything redrawn in place, such as the top card
and the hex editor's panels:

* `ui_patch_round_rect` erases only what an opaque rounded rect will not cover
  when it is redrawn: a frame around it, plus its own corners. Corners are
  alpha-blended, so redrawing one over its own last frame lets it creep toward
  the fill; left alone, a widget's corners visibly harden after a few dozen
  redraws.
* The erase must copy the real background. A flat colour standing in for the
  wallpaper leaves a visible box.

The setup wizard still repaints only what changed. Its widgets sit on a flat
background, so `patch_corners` there is a plain fill rather than a wallpaper
patch.

| Setup wizard interaction | Before | After |
|---|---|---|
| Language | 119,256 px | 30,616 px |
| Keyboard key | 117,816 px | 1,404 px |
| Accent swatch | 107,104 px | 10,944 px |
| User/date field | 103,440 px | 7,072 px |

## What each file is

The extension decides, except for `.bin`: an Aurora app container is a `.bin`
like any other, so the first four bytes are read and checked for `AOS1` or
`AUR1`. That check runs only over the `.bin` files in a folder, after the
listing is built, rather than opening every entry.

| Kind | Matches | Icon | A opens it with |
|------|---------|------|-----------------|
| Folder | directory | `files.png`, corners rounded | descends into it |
| Aurora app | `.bin` with `AOS1`/`AUR1` | `Aurora-Bin-File.png` | the app launcher |
| Image | `.png` `.jpg` `.jpeg` `.bmp` | `Media-File-Icon.png` | the viewer |
| Audio | `.wav` `.mp3` `.aaf` | `Media-File-Icon.png` | WAV plays; MP3 offers the hex editor; see below |
| Text | `.txt` `.log` | a ruled page, built from `Unkonwn File.png` | the text viewer |
| Binary | any other `.bin` | `Unknown-Bin-File.png` | "View HEX?", then the hex editor |
| Other | anything else | `Unkonwn File.png` | "View HEX?", then the hex editor |

Extensions match in any letter case, since long names keep the case they were
copied with.

List rows draw their icons at 24px, which leaves the 30px rows a 3px margin.
They used to draw the 32px art, which ran past the row edges; the opaque folder
tile showed it most.

## Text viewer

`text_view()` shows TXT and LOG files on the top screen, word-wrapped to its
width, twelve lines at a time.

The file is read whole, up to 2 MB, and then cleaned into UTF-8 the renderer can
draw. CRLF and a lone CR become newlines; tabs stay and line up on four-space
stops; other control bytes show as `.`; a character inside Latin-1 is kept and
one beyond it shows as `?`, since the fonts stop at U+00FF; and a stray high
byte is read as Latin-1, which covers most Windows-1252 text. Wrapping runs once
over the cleaned text, breaking at the last space that fits (or mid-word, for a
word wider than the screen), into an index of line starts. Scrolling redraws
twelve lines and never touches the card.

It borrows the image viewer's buffers, which are free while no picture is open:

| Buffer | Address | Holds |
|--------|---------|-------|
| File bytes | `0x25100000` | the file as read, 2 MB at most |
| Decoder scratch | `0x25700000` | the cleaned text, at most twice the file |
| Decoded RGB | `0x26100000` | the line index, 4 bytes a line |

D-pad up and down move a line; left and right, **L** and **R**, or the two
on-screen buttons move a page; **X** goes to the start and **Y** to the end;
**B** closes.

## Hex editor

Any file Aurora has no viewer for asks "File is Unknown or Unsupported. View
HEX?". **A** or the View HEX button opens `hex_edit()`.

The top screen shows twelve rows of eight bytes: the offset, the hex, and the
ASCII, with non-printing bytes as `.`. Only the 96 visible bytes are read, on a
page change, so a file of any size opens at once. Changed bytes are drawn in the
accent colour and the title bar counts them.

* The D-pad moves a byte, or a row up and down; **L** and **R** move a page.
* The 0-F keypad on the bottom screen types: the first digit sets the high half
  of the selected byte, the second the low half, then the cursor moves on. An
  underline marks the digit the next key sets.
* **A** enters edit mode, where up and down change the underlined digit and left
  and right move between digits. **A** or **B** leaves it.
* **START** or Save writes the changes after a confirmation; **SELECT** or Revert
  drops them; **B** or Close leaves, asking first if anything is unsaved.

Changes are held as a list of up to 1,024 (offset, value) pairs, and setting a
byte back to its original value removes its entry, so the count is of real
differences. Saving opens the file for writing and does one seek and one
single-byte write per change. Nothing is inserted or removed, so the file keeps
its size. If the card refuses the write, the editor says so and keeps the
changes, so the save can be tried again.

## Images

Everything decodes to packed RGB888 and is then drawn scaled down to fit, so
one scaler serves all three formats. Pictures already smaller than the screen
are drawn 1:1 rather than enlarged, which would only add blur; larger ones are
reduced by an integer factor with whole source pixels averaged, which avoids
the aliasing that sampling would give on hard edges.

Working memory sits between the app-return stubs and the ARM11 mailbox, clear
of the return descriptor at `0x25008000`, the OS snapshot at `0x26000000` (about
100KB) and the mailbox at `0x27000000`. It is live only while the viewer is
open.

| Buffer | Address | Size |
|--------|---------|------|
| File bytes | `0x25100000` | 6 MB |
| Decoder scratch | `0x25700000` | 8 MB |
| Decoded RGB | `0x26100000` | 8 MB |

That caps a picture at 2.7 million pixels, and any file at 6 MB. Larger ones
report "Image is too large" rather than failing part-way.

**BMP** covers uncompressed 8, 24 and 32bpp, both row orders. RLE and the 16bpp
bitfield forms are rejected rather than half-supported.

**PNG** covers every colour type (grey, RGB, palette, and both with alpha) at
bit depths 1, 2, 4, 8 and 16, with a DEFLATE decompressor in `image.c`.
Interlaced (Adam7) files are rejected. 16-bit samples are reduced to 8. There is
no alpha in the framebuffer, so transparency is composited onto the UI panel
colour while the image is converted.

**JPEG** covers baseline and extended sequential (SOF0/SOF1) at 8-bit precision,
1 or 3 components, 4:4:4 / 4:2:2 / 4:2:0, and restart intervals. Progressive
(SOF2), lossless, arithmetic-coded and hierarchical files are rejected with
"Unsupported variant". A truncated file keeps whatever decoded rather than
failing outright. Expect a second or two for a multi-megapixel photo, which is
why the explorer shows a "Decoding..." card first.

### How these were checked

None of this could be run on hardware while it was written, so each decoder was
transcribed line for line into Python and run against real files:

* **PNG**: the DEFLATE output was compared against `zlib` and the finished
  pixels against the project's own reference reader, over all 34 PNGs in
  `icons/`. Byte-identical on every one.
* **BMP**: generated files at 8, 24 and 32bpp, both row orders, at widths that
  are not multiples of four so the row padding is exercised.
* **JPEG**: decoded real photographs and compared the result by eye. This is
  what caught the one substantive bug: the IDCT's all-zero-AC shortcut skipped
  the rounding shift the general path applies, so flat blocks came out 1024
  times too large. Text stayed legible while everything else turned to noise.

## Audio

**WAV** plays uncompressed PCM at 8 or 16 bits, mono or stereo, 1-96 kHz. Two
conversions happen while the file is read, because the CSND channel Aurora
drives plays one mono stream of signed samples: 8-bit WAV is unsigned and is
biased into signed, and stereo is mixed down rather than having a channel
dropped, which would lose anything panned hard. Samples are converted a block at
a time straight into `AUDIO_PCM_ADDR`, so a long track never needs a second copy
of itself. Tracks longer than `AUDIO_PCM_MAX` are truncated.

**MP3** is recognised and labelled but not decoded. A Layer III decoder is a
large piece of work, and real-time 44.1 kHz stereo is not realistic on the ARM9
at its clock; the sensible home for one is the ARM11, which already owns CSND.

**`.aaf`** files are Aurora's own audio format and are played by the Music app.
