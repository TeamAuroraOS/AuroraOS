# The Home Menu: pages, folders and moving apps

| Piece | File |
|-------|------|
| The menu: drawing, input, moving, menus | `src/os/HomeMenu.c`, `include/homemenu.h` |
| The arrangement and its file | `src/os/HomeLayout.c`, `include/homelayout.h` |
| The apps it shows, and what opening one does | `src/os/os_main.c` (`scan_apps`, `build_home`, `home_open`) |
| The system menu (L + Down + Select) | `src/os/HomeMenu.c` (`home_system_menu`), `get_keys_down()` in `src/os/os_main.c`; in apps `src/os/SystemMenu11.c` and `include/system_menu.h` |
| Delete App | `MI_DELETE_APP` in `src/os/HomeMenu.c`, the `remove_app` hook (`home_remove_app` in `src/os/os_main.c`) |
| Wallpaper themes (Settings > Theme) | `theme_*` in `src/os/os_main.c`, `ui_theme_load` / `ui_theme_clear` in `src/ui.c` |
| Strings, in English, Spanish, French and Portuguese | `src/os/os_setup.c`, `include/lang.h` |
| Folder icon | `icons/folder-icon-without-text.png` (`ASSET_ICON_FOLDER_*`), `icon_folder_bits` in `include/icons.h` |

**Status: working on a New 3DS** (2026-10-03). Every behaviour below was also
run on the PC through the harness; see "What has been checked".

## What is on it

The bottom screen has a bar along the top and a grid of 5 x 3 tiles under it.

| Part | Does |
|------|------|
| Power icon, left of the bar | asks "Turn off the console?", then powers off |
| L + Down + Select | opens the system menu (Return to Home, Power Off, Cancel), on every screen and in apps; see "The system menu" |
| `< Name`, inside a folder | back to where the folder is |
| Settings icon, right of the bar | opens Settings, as START does |
| Grid | SD apps (from `SD:/Aurora/Apps`, and C apps from `SD:/Aurora/Apps/C`), Music, Files, 3D Model, aShop and folders |
| Dots under the grid | one per page, the current one wider; shown once there are two |

Power Off is no longer a tile. The top screen describes whatever has the focus:
an app, a folder (its first four icons, and how many things are in it), an empty
slot, or the bar button. An app with its own art (`HomeApp.asset_art`) shows
that alone instead of the icon and name card: aShop shows the auroraShop logo.

When aShop installs an app, the Home Menu rescans both app folders as aShop
closes and re-reads the layout file (`home_reload`), so the new app is there at
once, at the end of Home, and the folder and page shown stay as they were.

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
| Circle pad | the same as the D-pad, here and in every list and menu of the OS |
| L / R | previous / next page |
| A | opens the app or folder; on the bar, presses that button |
| B | in a folder, back out of it; on the bar, back to the grid |
| Y | the action menu for the selected tile or empty slot |
| X | the terminal |
| START | Settings |
| L + Down + Select | the system menu |

### Touch

| Touch | Does |
|-------|------|
| Tap a tile | opens it |
| Tap the power icon, `< Name` or the settings icon | as their buttons |
| Tap a dot | goes to that page |
| Swipe left or right | turns the page; the grid follows the stylus |
| Hold a tile (half a second) | picks it up: drag it, or let go without moving for the action menu |

### Sounds

Moving the selection clicks (move), opening something with A or a tap plays
select, and B or the back button plays back; a tap that opens a tile plays
only select. Y, START and X play select as they open their screen. Settings >
Menu sounds turns them off. Where else they play, and how:
[`audio.md`](audio.md), "Menu sounds".

The Home Menu has its own song, which starts when the startup sound is over and
loops at 70% volume under the clicks. It stops when anything opens from here and
when the lid closes, and restarts from the top on the way back; folders and
pop-ups are part of the Home Menu and do not stop it. Settings > Home music turns
it off. How it works: [`audio.md`](audio.md), "The Home Menu's song".

### The action menu (Y)

| Item | Does |
|------|------|
| Move | picks the tile up, to move with the D-pad |
| Move to... | a list of Home and the folders that can take it; it goes at the end there |
| New Folder | asks for a name, then puts the tile in a new folder in its place (on an empty slot, makes an empty folder) |
| Rename | a folder's name, on the keyboard |
| Remove Folder | after asking, if it is not empty: what it holds goes where the folder was |
| Delete App | on an SD app, asks "Delete this app?" with its name, deletes its `.bin` file, then offers to delete its data too |

Delete App is offered only for apps on the card (not Music, Files, 3D Model,
aShop or a folder). After "Delete this app?" it deletes the container, the
menu rescans both app folders and re-reads the layout (`home_reload`), and
the tile is gone. Then, if the app has a data folder on the card, a second
question names it, "Delete its data too?" with `SD:/...`, and **Delete** or
**Keep**:

* a C app's folder is `SD:/Aurora/Apps/C/<Name>/` (`aurora_c_data_dir()`,
  wherever the `.bin` was);
* any other app's is beside it and named after it, such as
  `SD:/Aurora/Apps/TETRIS/` for `Tetris.bin` (letter case does not matter);
  an app called `C.bin` has none, since that would be the C apps' folder;
* the question is not asked when another app on the card uses the same
  folder (two copies of one C app, one in each app folder).

Delete removes the folder with everything in it (`fo_delete()`); Keep leaves
it for later. aShop's record of the installed version stays either way, so
the store shows the app as not installed and offers it again. The deleted
app's line in `HomeMenu.txt` is dropped the next time the layout is saved. If
the card cannot be mounted or the file cannot be deleted (a read-only card,
for example), "Could not delete the app" says so and nothing changes; a data
folder that cannot be removed in full says "Could not delete all of its
data".

## The system menu

Pressing **L + Down + Select** together opens "Aurore Tools" on the bottom
screen, wherever you are: the Home Menu, Settings, Files, aShop, the terminal,
the setup wizard, or an app started from the Home Menu. The D-pad (or a tap)
picks a row, A chooses it, B closes the menu.

| Row | Does |
|-----|------|
| Return to Home | leaves every screen you are in and shows the Home Menu |
| Power Off | asks, then turns the console off |
| Cancel | closes the menu; the screen underneath is as it was |

Return to Home is left out before the Home Menu has started, in the setup
wizard and at the asset-pack warning, since there is no Home to go back to
yet. The chord does nothing while Settings > Wi-Fi or the Wi-Fi Test waits for
a Wi-Fi command, because the ARM11 core is busy and cannot present the menu.

### In the OS

`get_keys_down()` in `src/os/os_main.c` is the input path of every screen, so
it catches the chord there and calls `home_system_menu()`. That copies the
bottom framebuffer, draws the menu, and puts the copy back on Cancel.

Return to Home sets a request instead of jumping: until the Home Menu clears
it, `get_keys_down()` returns only **B**, and `anim_transition()` skips the
slide back. Each screen then leaves the way B normally makes it leave, closing
its files and saving what it saves, and the Home Menu's loop
(`home_navigation_requested()`) clears the request and redraws. A few screens
use B for something else, so they check the request themselves:

| Screen | B there | On Return to Home |
|--------|---------|-------------------|
| Terminal | Del | closes, as START does |
| On-screen keyboard (names, passwords, search) | backspace | cancels, as SELECT does |
| Wi-Fi firmware copy, the button code | one of the code's buttons | cancels |
| Hex editor with unsaved changes | asks "Discard changes?" | discards them without asking |
| Files | up one folder | goes up folder by folder, then leaves |

### In apps

An app has its own input loop, so the menu there is part of the ARM11 core
(`src/os/SystemMenu11.c`, from core 118). While an app started from the Home
Menu runs, the core reads the buttons itself, and on the chord draws the menu
in English with the 8x8 font into a spare bottom framebuffer and shows it. The
app's frame comes back when it closes. Power Off (with a Yes / Back question)
and Cancel are handled by the core, so they work in any app.

Return to Home cannot be done by the core alone. The app runs on the ARM9
with interrupts masked, so nothing on the ARM11 can take the ARM9 out of it;
only the app's own runtime can leave. The runtimes from Beta 7 (the C SDK 1.2
and later, and the Auric runtime of the same release) do this. At start they set
`app_caps` in the block at `0x233A8000` (`include/system_menu.h`). The core
then passes Return to Home to them through `action`, and the runtime leaves by
the normal HOME path at its next button read or present. While the menu is
open, they hold the app at its next button read.

An app built before Beta 7 has no `app_caps`. The menu still opens over it,
but choosing Return to Home shows "This app was built before Beta 7, so it
cannot be closed from here", and the way out is HOME, or rebuilding the app.
Such an app keeps polling HOME while the menu is up. If HOME is pressed then,
the app leaves and the return stub clears `app_active`; the core sees that
and closes the menu at once, so the restarting OS finds it answering.
`Games/Tetris.BIN` is built with the new runtime; the other Auric and C apps
in this tree get it when they are rebuilt with `aurc` or `sdk/aurcc.py`. An
app installed from aShop needs a new build from its developer.

## Wallpaper themes

**Settings > Theme** (the row after Accent colour) replaces the built-in
wallpaper on both screens with pictures from the card. A theme is a folder:

```
SD:/Aurora/themes/
`-- Night/
    |-- top.png       400 x 240
    `-- bottom.png    320 x 240
```

The screen lists **Aurora** (the built-in wallpaper) and then every folder
in `SD:/Aurora/themes` whose two files are PNGs of exactly those sizes, in
the card's directory order, five rows at a time. Up/Down choose, A or a tap
on a row applies it, B goes back. The Settings row shows the theme's folder
name, or "Aurora". The choice is saved in `SD:/Aurora/theme.txt` (the folder
name, or `@default` for the built-in wallpaper) and loaded again at every
boot and every return from an app. A folder that is no longer there or no
longer fits falls back to the built-in wallpaper.

Rules, checked by `theme_compatible()` from each PNG's header:

* both files must start with the PNG signature and be exactly 400 x 240 and
  320 x 240; nothing is scaled or cropped;
* the pictures should be opaque: transparent parts are blended onto the panel
  colour when the file is decoded;
* folder names up to 63 characters, without `/`, `\` or `:`, and not
  starting with `@` (kept for `@default`); longer ones are not listed, and at
  most 23 themes are;
* `SD:/Aurora/themes` is created at boot when it is missing.

A theme takes the place of the cached wallpaper (`ui_theme_load()` copies
the decoded pictures into the cache `ui_bg_build()` normally composes), so
every screen that draws the wallpaper shows it, and an accent colour no
longer tints it. Listing reads only the first 24 bytes of each picture, and
a saved theme costs two PNG decodes at boot. A picture whose header is right
but whose data does not decode shows "Could not load this theme" and the
wallpaper from before comes back. If `theme.txt` cannot be written (a
write-protected card) the theme still shows, but only until the next boot.
See also [`assets.md`](assets.md), "Themes".

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
A @store
```

`A` names an app: its file name in `SD:/Aurora/Apps` (`C/Bricks.bin` for one
in `SD:/Aurora/Apps/C`), or `@music`, `@files`,
`@model` and `@store` for the built-in screens. `F name` starts a folder and `E` ends
it. Lines that name an app no longer on the card are dropped; apps the file
does not mention (new ones) go at the end of Home, in name order. A folder that would be a third level is dropped
and what it holds stays in the folder around it. Without the file, Home holds
the SD apps in name order, then Music, Files, 3D Model and aShop. Deleting
the file resets the arrangement.

The Home Menu reads up to 160 SD apps, from both folders together.

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

On a New 3DS: all of it works. The system menu and the circle pad came later
(Beta 7) and were built, but not run in the harness or on a console here.
Delete App (PR 23) and wallpaper themes (PR 24, both 2026-10-20, reworked
2026-10-24) were checked with a host test of their code from `os_main.c` on
real FatFs and `FileOps.c` over a RAM disk (27 checks): which folders are
listed (wrong sizes, a missing picture, a JPEG named `.png`, an `@` name),
that listing decodes nothing, saving and loading `theme.txt` (with CRLF,
`@default`, a theme that is gone), a picture that fails to decode putting
the previous theme back, and Delete App's data folders (Delete, Keep, a
shared folder, `C.bin`, an app with no folder, a file already gone). The
screens themselves have not been run in the harness, and no console result is
recorded here.

## On the console

The menu needs the `Aurora/assets.pak` built with it (it holds the folder
icon); an older pack is refused, and the menu then draws its built-in icons.

1. The power icon is at the top left of the bottom screen and asks before
   turning off. There is no Power Off tile.
2. Press L + Down + Select in Settings > About and choose Return to Home; do
   the same in the terminal, in a rebuilt app (`Games/Tetris.BIN`), and in the
   keyboard of New Folder.
   Each should end on the Home Menu.
3. With more than fifteen apps the dots appear; swipe or press L/R.
4. Hold a tile, drag it, and let go somewhere else. Drag one onto the middle
   of another app to make a folder.
5. Y on a tile: New Folder, Rename, Move to..., Remove Folder.
6. Y on an SD app you can spare: Delete App. After the question the tile
   goes, and the others close up. For Tetris with its sounds, a second
   question offers `SD:/Aurora/Apps/TETRIS`; Keep leaves it.
7. Put a theme folder in `SD:/Aurora/themes`, pick it in Settings > Theme,
   then go back to Home and open an app and return: the pictures stay.
   Choose Aurora to go back.
8. Power off and back on: the arrangement and the theme are as you left them.

| Symptom | Likely cause |
|---------|--------------|
| Holding a tile never picks it up | the stylus wanders more than 8 pixels while held: `SLOP` in `HomeMenu.c` |
| Taps open the wrong tile | touch calibration (Settings > Touch Calibration) |
| Every arrangement is lost at power-off | the file could not be written: is the card read-only? |
| Icons look like plain shapes | an older `assets.pak` on the card (a boot message says it was not loaded) |
| Return to Home in an app says "This app was built before Beta 7" | rebuild it with `sdk/aurcc.py` or `aurc`; HOME still works |
| Settings > Theme says "No compatible themes found" | the folder is not directly in `SD:/Aurora/themes`, a file is not named `top.png` / `bottom.png`, is not a PNG, or is not exactly 400 x 240 / 320 x 240 |
| "Could not delete the app" | the card is write-protected, or the file was already gone |
| "Could not load this theme" | a picture's header says the right size but the data is damaged, or uses a PNG feature the decoder lacks (interlacing) |
