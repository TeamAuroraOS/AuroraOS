# The Home Menu: pages, folders and moving apps

| Piece | File |
|-------|------|
| The menu: drawing, input, moving, menus | `src/os/HomeMenu.c`, `include/homemenu.h` |
| The arrangement and its file | `src/os/HomeLayout.c`, `include/homelayout.h` |
| The apps it shows, and what opening one does | `src/os/os_main.c` (`scan_apps`, `build_home`, `home_open`) |
| The Aurora Menu (L + Down + Select, or the Power button twice) and the Dock | `src/os/HomeMenu.c` (`home_system_menu`, `dock_*`), `get_keys_down()` in `src/os/os_main.c`; in apps `src/os/SystemMenu11.c` and `include/system_menu.h`; see [`amenu.md`](amenu.md) |
| The top bar's shortcuts (aShop, Files, Music, Camera) | `BTN_*` in `src/os/HomeMenu.c`, the `shop`, `files`, `music` and `camera` hooks in `src/os/os_main.c` |
| Delete App | `MI_DELETE_APP` in `src/os/HomeMenu.c`, the `remove_app` hook (`home_remove_app` in `src/os/os_main.c`) |
| Themes (Settings > Theme) | `src/os/Theme.c`, `include/theme.h`; `theme_screen()` in `src/os/os_main.c`; `ui_theme_load_ex` / `ui_theme_clear` in `src/ui.c` |
| Strings, in English, Spanish, French and Portuguese | `src/os/os_setup.c`, `include/lang.h` |
| Folder icon | `icons/folder-icon-without-text.png` (`ASSET_ICON_FOLDER_*`), `icon_folder_bits` in `include/icons.h` |

**Status: working on a New 3DS** (2026-10-03). Every behaviour below was also
run on the PC through the harness; see "What has been checked".

## What is on it

The bottom screen has a bar along the top and a grid of 5 x 3 tiles under it.

| Part | Does |
|------|------|
| Power icon, left of the bar | asks "Turn off the console?", then powers off |
| L + Down + Select | opens the Aurora Menu, on every screen and in apps; see "The Aurora Menu" |
| `< Name`, inside a folder | back to where the folder is |
| Shop, Files, Music and Camera icons, right of the bar | open aShop, the File Explorer, the Music player and the Camera app ([`camera.md`](camera.md)) |
| Settings icon, at the right end of the bar | opens Settings, as START does |
| Grid | SD apps (from `SD:/Aurora/Apps`, and C apps from `SD:/Aurora/Apps/C`) and folders |
| Dots under the grid | one per page, the current one wider; shown once there are two |

Since Beta 8 the built-in screens are not tiles: aShop, Files, Music and the
Camera open from the bar, and the 3D Model viewer moved to Settings > 3D
Model Test. Power Off is not a tile either. The top screen describes
whatever has the focus: an app (or its 3D banner), a folder (its first four
icons, and how many things are in it), an empty slot, or the bar button.

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
| D-pad | moves the selection; past the left or right edge it turns the page; up from the top row goes to the bar's Settings icon, and Left / Right walk along the bar |
| Circle pad | the same as the D-pad, here and in every list and menu of the OS |
| L / R | previous / next page |
| A | opens the app or folder; on the bar, presses that button |
| B | in a folder, back out of it; on the bar, back to the grid |
| Y | the action menu for the selected tile or empty slot |
| X | the terminal |
| START | Settings |
| L + Down + Select | the Aurora Menu |
| Power button | once: "Turn off the console?"; twice within 300 ms: the Aurora Menu |

### Touch

| Touch | Does |
|-------|------|
| Tap a tile | opens it |
| Tap a bar icon or `< Name` | as their buttons |
| Tap a dot | goes to that page |
| Swipe left or right | turns the page; the grid follows the stylus. On a tile, only a quick sideways flick (under 160 ms, mostly horizontal, with more than one page) swipes |
| Touch a tile and move | picks it up at once and drags it |
| Hold a tile still (0.22 s) | picks it up: drag it, or let go without moving for the action menu |

Animated (AIC1) icons now animate wherever they show on the page, also inside
folder tiles, not only under the selection.

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
| Pin to Dock | on an SD app, adds it to the Aurora Menu's Dock (up to three); on a pinned app it reads Unpin, and with three pinned it reads "Dock Full (3/3)" and is greyed out; see [`amenu.md`](amenu.md) |

The buttons are a little smaller than before so that seven fit. Delete App is
offered only for apps on the card (not for a folder). After "Delete this
app?" it deletes the container, the
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

## The Aurora Menu

Pressing **L + Down + Select** together opens the Aurora Menu wherever you
are: the Home Menu, Settings, Files, aShop, the terminal, the setup wizard,
or an app started from the Home Menu. On the Home Menu, pressing the Power
button twice quickly opens it too. It replaced the old "Aurore Tools" menu
(Return to Home, Power Off, Cancel) in Beta 8: a sidebar on the left of the
bottom screen with plugin rows, Home, Brightness, Reload /amenu and Power
Off, and a Dock of up to three pinned apps on the right. B closes it. The
whole menu, the Dock and plugins are described in [`amenu.md`](amenu.md);
this section covers how Home leaves the screens.

Home is left out before the Home Menu has started, in the setup wizard and at
the asset-pack warning, since there is no Home to go back to yet. The chord
does nothing while Settings > Wi-Fi or the Wi-Fi Test waits for a Wi-Fi
command, because the ARM11 core is busy and cannot present the menu.

### In the OS

`get_keys_down()` in `src/os/os_main.c` is the input path of every screen, so
it catches the chord there and calls `home_system_menu()`. That copies the
bottom framebuffer, draws the menu over a dimmed copy of it, and puts the
copy back when the menu closes.

Home sets a request instead of jumping: until the Home Menu clears
it, `get_keys_down()` returns only **B**, and `anim_transition()` skips the
slide back. Each screen then leaves the way B normally makes it leave, closing
its files and saving what it saves, and the Home Menu's loop
(`home_navigation_requested()`) clears the request and redraws. A few screens
use B for something else, so they check the request themselves:

| Screen | B there | On Home |
|--------|---------|-------------------|
| Terminal | Del | closes, as START does |
| On-screen keyboard (names, passwords, search) | backspace | cancels, as SELECT does |
| Wi-Fi firmware copy, the button code | one of the code's buttons | cancels |
| Hex editor with unsaved changes | asks "Discard changes?" | discards them without asking |
| Files | up one folder | goes up folder by folder, then leaves |

### In apps

An app has its own input loop, so the menu there is part of the ARM11 core
(`src/os/SystemMenu11.c`, from core 118, rewritten for core 150). While an
app started from the Home Menu runs, the core reads the buttons itself, and on
the chord draws the menu in English, with the OS's Figtree fonts (or the 8x8
font without the asset pack), into the two spare bottom framebuffers in turn
over a dimmed copy of the app's frame. The app's frame comes back when it
closes. Power Off (with a Power Off / Cancel page), Brightness and plugins
are handled by the core, so they work in any app.

Home (Return to Home) cannot be done by the core alone. The app runs on the ARM9
with interrupts masked, so nothing on the ARM11 can take the ARM9 out of it;
only the app's own runtime can leave. The runtimes from Beta 7 (the C SDK 1.2
and later, and the Auric runtime of the same release) do this. At start they set
`app_caps` in the block at `0x233A8000` (`include/system_menu.h`). The core
then passes Return to Home to them through `action`, and the runtime leaves by
the normal HOME path at its next button read or present. While the menu is
open, they hold the app at its next button read.

An app built before Beta 7 has no `app_caps`. The menu still opens over it,
but choosing Home (or a Dock app) shows "Built before Beta 7. Press HOME
after closing, or rebuild with the SDK.", and the way out is HOME, or
rebuilding the app.
Such an app keeps polling HOME while the menu is up. If HOME is pressed then,
the app leaves and the return stub clears `app_active`; the core sees that
and closes the menu at once, so the restarting OS finds it answering.
`Games/Tetris.BIN` is built with the new runtime; the other Auric and C apps
in this tree get it when they are rebuilt with `aurc` or `sdk/aurcc.py`. An
app installed from aShop needs a new build from its developer.

## Themes

**Settings > Theme** (the row after Accent colour) changes the look of the
Home Menu. Since Beta 8 (`src/os/Theme.c`, PR 51) a theme can bring
pictures, colours, its own Home Menu song and its own menu sounds. A theme is
a folder:

```
SD:/Aurora/themes/
`-- NorthernLights/
    |-- top.png        or top.jpg / top.jpeg, any size
    |-- bottom.png     or bottom.jpg / bottom.jpeg, any size
    |-- bgm.wav        the Home Menu's song
    |-- select.wav     menu sounds
    |-- back.wav
    |-- move.wav
    `-- theme.ini      name, author and colours
```

Every file is optional; a folder is listed when it holds at least one of
them. The repository has an example, `Aurora/themes/NorthernLights` (by
Moonii, with a zip of the same files beside them).

| File | What it does |
|------|--------------|
| `top.*`, `bottom.*` | the wallpaper of that screen. A picture of exactly 400 x 240 or 320 x 240 is copied as it is; any other size is scaled to cover the screen and cropped in the middle. Without one (or if it does not decode) that screen gets the built-in pattern over a gradient from `bg_top` to `bg_bottom` |
| `bgm.wav` | replaces the Home Menu's song (8- or 16-bit PCM, made mono; up to the song buffer's 5 MB, about 60 s at 44.1 kHz) |
| `select.wav`, `back.wav`, `move.wav` | replace those menu sounds; the three share the 256 KB menu-sound buffer, and a missing or bad one keeps the built-in sound |
| `theme.ini` | `key=value` lines; `#`, `;` and `[` start comments; the first 2 KB are read |

`theme.ini` keys (colours as `#RRGGBB`, the `#` optional):

| Key | Used for |
|-----|----------|
| `name`, `author` | shown in Settings > Theme (`name` instead of the folder name) |
| `accent` | the accent colour while the theme is on; without it, the accent from Settings > Accent colour |
| `bg_top`, `bg_bottom` | the gradient behind a screen with no picture |
| `panel_top`, `panel_bottom` | the top screen's info card, the Aurora Menu's panels, the Camera app |
| `slot_top`, `slot_bottom` | app tiles, and the Aurora Menu's rows |
| `slot_empty_top`, `slot_empty_bottom` | empty slots |
| `folder_top`, `folder_bottom` | folder tiles |
| `status_bar`, `text`, `text_dim` | the status bar and the text on the cards |

The colours reach the Home Menu, the status bar, the Camera app and, through
the shared block, the Aurora Menu in apps (`theme_sync_menu_accent()`).
Settings, Files and the other screens keep their built-in colours.

The screen lists **Aurora (Default)** and then every theme folder, in the
card's directory order, four rows at a time: the theme's name, and under it
what it has (`TOP BOT BGM SFX COL`) or its author. Up/Down choose, A or a tap
on a row applies it, B goes back. Applying it reloads the wallpaper, the
palette, the song and the menu sounds at once. The Settings row shows the
theme's name, or "Aurora". The choice is saved in `SD:/Aurora/theme.txt` (the
folder name, or `@default`) and loaded at every boot, which includes every
return from an app.

Rules:

* folder names up to 63 characters, without `/`, `\` or `:`, and not starting
  with `@` (kept for `@default`); at most 23 themes are listed;
* `SD:/Aurora/themes` is created at boot when it is missing;
* "Could not load this theme" shows only when the folder no longer has any
  theme file; a picture that does not decode just leaves that screen to the
  gradient;
* a theme whose `theme.txt` cannot be written (a write-protected card) shows
  until the next boot.

A theme takes the place of the cached wallpaper (`ui_theme_load_ex()` fills
the cache `ui_bg_build()` normally composes), so every screen that draws the
wallpaper shows it. Listing reads each folder's file names and its
`theme.ini`, not the pictures. See also [`assets.md`](assets.md), "Themes".

## Moving things

### With the stylus

Touch a tile and move the stylus, or hold it still until it lifts, then drag
it. The other tiles move aside to show where it will go. Where the stylus is
on a tile decides what happens:

| Stylus over | Lifting there |
|-------------|---------------|
| The left or right fifth of a tile | puts it before or after that tile |
| The middle of a folder that can take it | puts it in the folder, at the end; the folder lifts and the carried tile shrinks into it |
| The middle of an app, where a new folder is allowed | makes a folder of the two, named "Folder" |
| An empty slot after the last tile | puts it at the end |

Held over the middle of a folder for 0.45 s, the folder opens with the tile
still carried, to place it at an exact spot inside. Inside a folder the bar
turns into "Drag here to move out of folder" while a tile is carried: let go
over it to put the tile in the folder's parent, just after the folder, or
hold it there for 0.35 s and the menu goes back out of the folder, still
carrying. Held at the left or right side of the screen for 0.45 s, the page
turns. A tile that
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
A Bricks.bin
```

`A` names an app: its file name in `SD:/Aurora/Apps` (`C/Bricks.bin` for one
in `SD:/Aurora/Apps/C`). Before Beta 8 the built-in screens had lines too
(`@music`, `@files`, `@model`, `@store`); they are no longer apps, so such
lines are dropped like any app that is gone. `F name` starts a folder and `E`
ends it. Lines that name an app no longer on the card are dropped; apps the file
does not mention (new ones) go at the end of Home, in name order. A folder that would be a third level is dropped
and what it holds stays in the folder around it. Without the file, Home holds
the SD apps in name order. Deleting the file resets the arrangement.

The Dock's pinned apps are kept apart, in `SD:/Aurora/pinned.txt`
([`amenu.md`](amenu.md)).

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

On a New 3DS: all of it works (as it was in Beta 7). The system menu and the
circle pad came later (Beta 7) and were built, but not run in the harness or
on a console here.
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

Beta 8 (PR 51, 2026-11-23) changed much of this file's subject: the bar's
shortcuts, the built-in screens leaving the grid, Pin to Dock, the Aurora
Menu, the Power button, the shorter hold and drag times, dropping on the bar
to leave a folder, and the new themes. It builds; none of it has been run in
the harness, and no console result is recorded here. The old theme host test
checked code that has since been replaced by `Theme.c`.

## On the console

The menu draws its art from `Aurora/assets.pak`. Since Beta 8 a pack with
fewer entries (an older one) loads too, and the icons it lacks (the camera
and the bar's Files and Shop icons) fall back to their built-in drawings; a
pack with more entries than the build knows also loads, the extra ones
skipped (see [`assets.md`](assets.md)).

1. The power icon is at the top left of the bottom screen and asks before
   turning off. There is no Power Off tile. Press the Power button once (the
   same question) and twice quickly (the Aurora Menu).
2. Press L + Down + Select in Settings > About and choose Home; do
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
7. Put `Aurora/themes/NorthernLights` on the card, pick it in Settings >
   Theme: the pictures, colours, song and clicks change at once. Open an app
   and return: they stay. Choose Aurora (Default) to go back.
8. Y on an app > Pin to Dock, then open the Aurora Menu and press Right: the
   app is in the Dock, and A opens it. Try it from inside an app too.
9. The bar's Shop, Files, Music and Camera icons open their screens.
10. Power off and back on: the arrangement, the Dock and the theme are as you
    left them.

| Symptom | Likely cause |
|---------|--------------|
| A tile lifts when you meant to swipe | a swipe that starts on a tile must be a quick sideways flick (under 160 ms); start it between tiles, or use L / R |
| Taps open the wrong tile | touch calibration (Settings > Touch Calibration) |
| Every arrangement is lost at power-off | the file could not be written: is the card read-only? |
| Icons look like plain shapes | an older `assets.pak` on the card (a boot message says it was not loaded) |
| Home in an app says "Built before Beta 7" | rebuild it with `sdk/aurcc.py` or `aurc`; HOME still works |
| Settings > Theme lists only Aurora (Default) | the folder is not directly in `SD:/Aurora/themes`, or holds none of the file names above |
| "Could not delete the app" | the card is write-protected, or the file was already gone |
| A theme's screen shows the gradient instead of its picture | the picture did not decode (a damaged file, an interlaced PNG or a progressive JPEG) |
| "Could not load this theme" | the folder was emptied or removed after the list was made |
