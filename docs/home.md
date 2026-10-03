# The Home Menu: pages, folders and moving apps

| Piece | File |
|-------|------|
| The menu: drawing, input, moving, menus | `src/os/HomeMenu.c`, `include/homemenu.h` |
| The arrangement and its file | `src/os/HomeLayout.c`, `include/homelayout.h` |
| The apps it shows, and what opening one does | `src/os/os_main.c` (`scan_apps`, `build_home`, `home_open`) |
| Strings, in English, Spanish and French | `src/os/os_setup.c`, `include/lang.h` |
| Folder icon | `icons/Folder.png` (`ASSET_ICON_FOLDER_*`), `icon_folder_bits` in `include/icons.h` |

**Status: working on a New 3DS** (2026-10-03). Every behaviour below was also
run on the PC through the harness; see "What has been checked".

## What is on it

The bottom screen has a bar along the top and a grid of 5 x 3 tiles under it.

| Part | Does |
|------|------|
| Power icon, left of the bar | asks "Turn off the console?", then powers off |
| `< Name`, inside a folder | back to where the folder is |
| Settings icon, right of the bar | opens Settings, as START does |
| Grid | SD apps (from `SD:/Aurora/Apps`), Music, Files, 3D Model and folders |
| Dots under the grid | one per page, the current one wider; shown once there are two |

Power Off is no longer a tile. The top screen describes whatever has the focus:
an app, a folder (its first four icons, and how many things are in it), an empty
slot, or the bar button.

### Pages

Home and every folder show fifteen things to a page, in order. When a page is
full the next thing starts a new page, and a page that empties goes away, so
there are never gaps. Home and each folder hold up to 12 pages, 180 things.

### Folders

A folder holds apps and, if it is in Home, other folders. The rule is two
levels at most: **Home > folder > folder**. A folder inside a folder cannot
hold a folder, and a folder that holds a folder cannot go into another folder.
Home can hold as many folders as fit, and so can a folder in Home. Everything
that would break the rule is refused: the menu greys out the action, a drag
does not offer it, and "Move to..." does not list the place. Up to 64 folders
exist at once; names are up to 24 characters.

A folder's tile shows small icons of the first four things in it, or a folder
icon when it is empty.

## Controls

### Buttons

| Button | Does |
|--------|------|
| D-pad | moves the selection; past the left or right edge it turns the page; up from the top row goes to the bar |
| L / R | previous / next page |
| A | opens the app or folder; on the bar, presses that button |
| B | in a folder, back out of it; on the bar, back to the grid |
| Y | the action menu for the selected tile or empty slot |
| X | the terminal |
| START | Settings |

### Touch

| Touch | Does |
|-------|------|
| Tap a tile | opens it |
| Tap the power icon, `< Name` or the settings icon | as their buttons |
| Tap a dot | goes to that page |
| Swipe left or right | turns the page; the grid follows the stylus |
| Hold a tile (half a second) | picks it up: drag it, or let go without moving for the action menu |

### The action menu (Y)

| Item | Does |
|------|------|
| Move | picks the tile up, to move with the D-pad |
| Move to... | a list of Home and the folders that can take it; it goes at the end there |
| New Folder | asks for a name, then puts the tile in a new folder in its place (on an empty slot, makes an empty folder) |
| Rename | a folder's name, on the keyboard |
| Remove Folder | after asking, if it is not empty: what it holds goes where the folder was |

## Moving things

### With the stylus

Hold a tile until it lifts, then drag it. The other tiles move aside to show
where it will go. Where the stylus is on a tile decides what happens:

| Stylus over | Lifting there |
|-------------|---------------|
| The left or right third of a tile | puts it before or after that tile |
| The middle of a folder that can take it | puts it in the folder, at the end; the folder lifts and the carried tile shrinks into it |
| The middle of an app, where a new folder is allowed | makes a folder of the two, named "Folder" |
| An empty slot after the last tile | puts it at the end |

Held over the middle of a folder for 0.7 s, the folder opens with the tile
still carried, to place it at an exact spot inside. Held over `< Name` in the
bar for 0.6 s, the menu goes back out of the folder, still carrying. Held at
the left or right side of the screen for 0.55 s, the page turns. A tile that
cannot be put where it is let go goes back, with a message saying why; B
while dragging puts it back too.

### With the D-pad

Choose **Move** from the Y menu. The tile lifts; the D-pad moves it (across
pages at the edges, L and R a page at a time), **A** puts it down and **B**
puts it back. Touching the screen while carrying hands it to the stylus. To put
a tile into a folder with the buttons, use **Move to...**.

## The layout file

Every change is saved to **`SD:/Aurora/HomeMenu.txt`** straight away. One line
per thing, in order:

```
# Aurora Home Menu: A app, F folder, E end of folder
F Games
A Tetris.bin
F Puzzles
A Sudoku.bin
E
E
A @music
A @files
A @model
```

`A` names an app: its file name in `SD:/Aurora/Apps`, or `@music`, `@files`
and `@model` for the built-in screens. `F name` starts a folder and `E` ends
it. Lines that name an app no longer on the card are dropped; apps the file
does not mention (new ones) go at the end of Home, in name order. A folder that would be a third level is dropped
and what it holds stays in the folder around it. Without the file, Home holds
the SD apps in name order, then Music, Files and 3D Model, as before. Deleting
the file resets the arrangement.

The Home Menu reads up to 160 SD apps.

## How it works

`HomeLayout.c` keeps Home and each folder as an ordered list of entries (an
app's number, or a folder), with each folder's parent. `hl_fits()` is the one
place the rules live: room in the container, not into itself, and the depth of
the container plus the height of what goes in (0 for an app, 1 for a folder, 2
for a folder holding a folder) no more than 2.

`HomeMenu.c` shows one container at a time. Each tile's position is a spring
(`AnimVal`) across all the pages laid side by side, and one more spring
scrolls that strip, so turning a page, reflowing round a carried tile and
dropping one all animate the same way. A carried tile is taken out of the
layout and the view shows a gap where it would go; letting go puts it in, or
back where it came from. The touch input is a small state machine: press,
then a tap, a swipe or a hold; a hold becomes a drag.

## What has been checked

On the PC, with the real menu code, 23 SD apps and scripted buttons and stylus:

* **The rules**, in a unit test of `HomeLayout.c`: depth limits for new
  folders, wrapping, merging and moving folders; refusing a folder into
  itself; 180 per container; 64 folders; unfolding in place; the file round
  trip, and a hand-edited file with CRLF line ends, a third level, an unknown
  app, a duplicate and an unnamed folder.
* **The screens:** pages and dots, D-pad and L/R paging, the bar focus and
  its previews, swiping, tapping a folder, the back button and the power
  question; New Folder with the keyboard; dragging to reorder, into a folder,
  onto an app; holding for the menu; a folder opening under a held tile;
  carrying out through the back button; turning the page at the screen edge;
  the D-pad Move; the depth rule in the menu, in "Move to..." and while
  dragging; Rename and Remove Folder.

On a New 3DS: all of it works.

## On the console

The menu needs the `Aurora/assets.pak` built with it (it holds the folder
icon); an older pack is refused, and the menu then draws its built-in icons.

1. The power icon is at the top left of the bottom screen and asks before
   turning off. There is no Power Off tile.
2. With more than fifteen apps the dots appear; swipe or press L/R.
3. Hold a tile, drag it, and let go somewhere else. Drag one onto the middle
   of another app to make a folder.
4. Y on a tile: New Folder, Rename, Move to..., Remove Folder.
5. Power off and back on: the arrangement is as you left it.

| Symptom | Likely cause |
|---------|--------------|
| Holding a tile never picks it up | the stylus wanders more than 8 pixels while held: `SLOP` in `HomeMenu.c` |
| Taps open the wrong tile | touch calibration (Settings > Touch Calibration) |
| Every arrangement is lost at power-off | the file could not be written: is the card read-only? |
| Icons look like plain shapes | an older `assets.pak` on the card |
