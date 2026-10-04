# aShop

aShop ("auroraShop" on its logo) is Aurora's app store. This first
version is a demo: it browses a catalogue of apps and news, shows each app's
page, and runs downloads into `SD:/Aurora/Apps`, but there is no download
server yet, since Aurora's Wi-Fi stops at DNS and ping (no TCP). Everything
around the network is real, so the transport and a fetched catalogue can be
plugged in later without changing the screens.

The screens follow the mock-ups: the welcome screen (`shop-home`), the banner
carousel (`shop-main-bottom` with the `shop-main-icon-*`, `shop-promo-1` and
`shop-update` banners), the app page (`apps-shop`), the download screen
(`downloading`) and the Home Menu preview (`topscreen-home-menu`,
`topscreen-icon`).

## Opening it

aShop is a tile on the Home Menu. On a card that already has a
`HomeMenu.txt`, a new built-in tile goes at the end of Home. With the tile
selected, the top screen shows the auroraShop logo in place of the usual icon
and name card.

## Screens

| Screen | Top | Bottom |
|--------|-----|--------|
| Main | striped green background, the shopping bag, "Welcome to aShop!" | the section's name, the banner carousel, and Search / Go! / Options |
| Section | the selected item's page | the section's apps or news as a list |
| Page | the item's page, scrollable | an app's icon, version, size and state, with Download (or Update, or Download again); or a news item |
| Download | a checkerboard floor, the app's card and a progress bar | the percentage, then the result, with Cancel or OK |
| Search | the bag and "Search aShop" | the on-screen keyboard; then the results as a section |
| Options | | Check for updates, Reload catalogue, About aShop |

The status bar on aShop's top screen is taller than the Home Menu's, in the
store's green, with "aShop (2 new updates)" after the clock and, at the right,
the Home Menu's grid and aShop's own icon underlined as the app in use. While a
download runs it reads "aShop (Downloading software...)", in the page's dark
grey and without the icons. It is `status_bar_draw_app()` in
`src/os/StatusBar.c`, for any full-screen app to use.

## Controls

| Where | Buttons | Touch |
|-------|---------|-------|
| Main | Left/Right or L/R move along the banners, **A** opens one, **Y** Search, **X** Options, **B** or START back to the Home Menu | swipe the banners, tap a side banner to move to it, tap the middle one to open it, tap the bar's buttons |
| Section | Up/Down pick, Left/Right jump five, **A** opens, **Y** Search, **B** back | tap a row to pick it, tap it again to open it, tap Back / Go! / Search |
| Page | Up/Down (or the Circle Pad) scroll the top screen, **A** downloads, **B** back | tap Back or Download |
| Download | **B** cancels; when it ends, **A** or **B** | tap Cancel, then OK |
| Options | Up/Down and **A**; **B** or **X** closes | tap an item; a tap outside the card closes it |

## On the SD card

```
SD:/Aurora/Store/
|-- catalog.txt     optional: replaces the built-in demo catalogue
|-- bag.png         the welcome screen's bag, 92x94
|-- featured.png    banners, 150x108, one per section (named by the catalogue)
|-- news.png
|-- apps.png
|-- games.png
|-- new3ds.png
|-- updates.png
|-- installed.txt   written by aShop: "file version" for each app it put on the card
`-- Packages/       optional: app containers to install, by their file names
```

`Aurora/Store/` in the repository holds the demo's art; copy it to the card
with the rest of `Aurora/`. `new3ds.png`, the "Better on New 3DS" banner, shows
Nintendo's logo and a photo of the console, so it is git-ignored and stays on
the machine it was made on, like the rest of that material.

Everything is optional. Without the folder aShop runs on the built-in
catalogue, draws each banner as a plain tile in the section's colour with its
name, and shows its own icon in place of the bag. Without a card it still
browses, and a download says there is no SD card.

Banners and icons are decoded once, when aShop opens, at the size they are
drawn (`image_decode_fit`), and a banner's corners are rounded then. Art of
another size is scaled, so the files can be any size, but drawing them at
their own size keeps opening quick. PNG and baseline JPEG both work.

## The catalogue

`SD:/Aurora/Store/catalog.txt` replaces the built-in demo when it is there.
It is plain text, UTF-8 (letters up to U+00FF draw), with Windows or Unix line
ends:

```
# a comment
[section featured]
title=Featured Software
banner=featured.png
color=2EC48A

[section updates]
title=Download updates
banner=updates.png
kind=updates

[app tetris]
name=Tetris
dev=Aurora
version=1.1
size=28616
file=Tetris.bin
icon=tetris.png
in=featured games
text=The first paragraph of the page.
text=
text=Each text= line is one line of the page; an empty one leaves a gap.

[news welcome]
title=Welcome to aShop
date=Oct 2026
text=News items are pages with no download.
```

| Record | Key | Meaning |
|--------|-----|---------|
| `[section id]` | `title` | the name on the title card (default: the id) |
| | `banner` | a picture in `SD:/Aurora/Store` |
| | `color` | `RRGGBB` for the plain tile drawn without a banner |
| | `kind` | `apps` (the default: apps whose `in` names this section), `news` (every news item) or `updates` (every app with an update) |
| `[app id]` | `name`, `dev`, `version` | shown on the page and in lists |
| | `size` | bytes, or with `K` or `M`: `512K`, `100M` |
| | `file` | its name in `SD:/Aurora/Apps` (default: the id plus `.bin`) |
| | `icon` | a picture in `SD:/Aurora/Store`, drawn at 48 and 24 pixels; without one, a grey tile with the console icon |
| | `in` | the sections it is listed in, separated by spaces |
| | `text` | the page, one line per key |
| `[news id]` | `title`, `date`, `text` | as for an app |

Sections appear on the carousel in the order written, and aShop starts on the
one with the id `featured`. Limits: 8 sections, 48 apps, 16 news items, a
32 KB file and 24 KB of page text in all; anything past them is left out.

## Installed apps and updates

An app counts as installed when its `file` is in `SD:/Aurora/Apps`, which is
what the Home Menu shows too. `installed.txt` remembers the version aShop last
put there, and an installed app has an update when that version differs from
the catalogue's, or when aShop has no record of it (it was copied by hand).
The count is in the status bar, the Download updates section lists them, and
Options > Check for updates re-reads the card first.

## Downloads in the demo

A download is paced at 3 MB a second, and never shorter than 2.5 seconds, so
its screen can be followed (`DEMO_RATE`, `DEMO_MIN_MS` in `Store.c`):

* When `SD:/Aurora/Store/Packages/<file>` exists, it is the download. Its
  header must be an Aurora container (AOS1 or AUR1). It is copied to
  `SD:/Aurora/Apps/<file>.part`, which the Home Menu ignores, and renamed over
  any old copy only when complete, so a cancelled or failed download leaves the
  old app as it was. The version is recorded, and when aShop closes the Home
  Menu rescans the apps and shows the new one where the layout file says (new
  apps at the end of Home), without a restart.
* Without a package only the progress runs, for the catalogue's `size`. An app
  already on the card is then recorded at the new version ("Up to date"), so
  the update flow can be tried; nothing is written to `SD:/Aurora/Apps`, so an
  app that was not there is not shown as installed.

To try a real install, copy `Games/Tetris.BIN` or `sample/RAINBOW.BIN` into
`SD:/Aurora/Store/Packages/` as `Tetris.bin` or `Rainbow.bin`.

## Making it real

What changes when there is a server:

* **The catalogue.** Fetch it and the banners into `SD:/Aurora/Store` before
  `store_load()` reads them; the file already overrides the demo, and the card
  copy is what aShop shows when offline. Remove `demo_catalog` then.
* **The transport.** `dl_start()` opens the source and `dl_step()` moves at
  most what is due each frame, into `.part`. A network source replaces the
  package file there; the pacing goes, and `done` and `total` come from the
  transfer. The header check, `.part` and rename, the record, the result screen
  and the Home Menu rescan stay as they are.
* **Trust.** A real store should check what it downloads (a hash or signature
  in the catalogue) before the rename.

## Memory

| Address | Size | Use |
|---------|------|-----|
| `0x26E00000` | 2 MB | `STORE_ARENA_ADDR`: decoded banners, the bag and app icons (about 0.4 MB for the demo), only while aShop is open |
| `0x25100000` | 2 MB | the picture file being decoded, then the decoder's rows: the image viewer's buffers, idle while aShop is open |
| `0x25700000`, `0x26100000` | | the decoder's own scratch, as for the image viewer |

The bar strip kept for scrolling (43 KB), the catalogue (32 KB) and its text
(24 KB) are in the OS's `.bss`.

## Art

* In the asset pack (`tools/mkassets.py`): aShop's icon (`store.png`) at 64,
  32 and 16 pixels, the auroraShop logo (`icons/store-logo.png`, 191x152, the
  `ART` list), and two masks drawn for aShop in the icon set's style (80x80, a
  7-pixel rounded stroke): `Search.png` and `Wrench.png`.
* The welcome screen's stripes and the download screen's floor are drawn in
  code: the stripes from a 16-row colour table that matches the mock-up, the
  floor as a checkerboard plane in perspective (four samples a pixel, fading in
  toward the horizon), which moves slowly toward the viewer while a download
  runs.

## Code map

| Part | Where in `src/os/Store.c` |
|------|---------------------------|
| Catalogue: parse, records, states | `parse`, `catalog_load`, `records_load`/`records_save`, `states_update` |
| Art | `art_load`, `round_art`, `store_load` |
| Drawing | `stripes`, `checker_floor`, `shade_round`, `buttons`, `tile`, `page_top`, `wrap` |
| Screens | `main_screen`, `list_screen`, `page_screen`, `download_screen`, `search_screen`, `options_menu` |
| Download | `dl_start`, `dl_step`, `dl_finish`, `dl_close` |

`store_screen()` returns how many apps it installed; `home_open()` in
`src/os/os_main.c` then rescans the apps and calls `home_reload()`
(`src/os/HomeMenu.c`), which re-reads the layout file and keeps the page shown.
The logo preview is `HomeApp.asset_art`.

## Checked on the PC

With the host harness (the real OS code on the PC, frames dumped per present):
the Home preview, the welcome screen and carousel against the mock-ups, swipes
and taps on the carousel, every list, page and button by keys and by touch, a
real install of Rainbow from `Packages` (the tile then appears on the Home
Menu's second page), a simulated update of Tetris, a 100 MB simulated download
cancelled part way, Options (updates, reload, About), the news reader with
scrolling, search with the keyboard, a catalogue on the card with CRLF line
ends and an icon, and the card without the Store folder. The Home Menu's status
bar is pixel-identical to before.

## Console checklist

* The aShop tile and the logo preview on the Home Menu.
* Opening aShop: how long before it slides in (it decodes the banners first).
* The carousel by D-pad, L/R and swipes; the title card fading in.
* A list, a page, scrolling a long page with the D-pad and the Circle Pad.
* A download with a package in `Packages`, then the app on the Home Menu.
* The download screen's floor and bar at full speed.
* Search, Options, About.
