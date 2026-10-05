# aShop

aShop ("auroraShop" on its logo) is Aurora's app store. It shows the apps and
games published on the Aurora Network, with news, and downloads them into
`SD:/Aurora/Apps`, where the Home Menu shows them. The catalogue, the icons and
the apps come from the account server (the `aurora-site/Account-API`
repository; its `API.md`, section *aShop*, is the contract), so aShop needs an
Aurora account linked to the console (see [`account.md`](account.md)).

Status: works on the PC against the real server under `wrangler dev` (see
*What was checked*). It needs the server version with aShop deployed before it
works on a console.

The screens follow the mock-ups: the welcome screen (`shop-home`), the banner
carousel (`shop-main-bottom` with the `shop-main-icon-*`, `shop-promo-1` and
`shop-update` banners), the app page (`apps-shop`), the download screen
(`downloading`) and the Home Menu preview (`topscreen-home-menu`,
`topscreen-icon`).

aShop's own screens follow the language chosen in setup (English, Spanish or
French); the catalogue's text (app pages, section names, news) is shown as the
server sends it.

## Opening it

aShop is a tile on the Home Menu. With the tile selected, the top screen shows
the auroraShop logo in place of the usual icon and name card.

- **No account linked:** "Aurora account required" with *Back* and *Link an
  account*. *Link an account* opens the same screen as Settings > Aurora
  Account; once the console is linked, aShop carries on.
- **Linked:** "Connecting to aShop..." on the welcome screen, with a spinner.
  The console joins the network saved in Settings > Wi-Fi when it is not on
  one, fetches the catalogue (or learns that its copy is current), then any
  icons it does not have yet ("Getting icons (1/2)"), and opens on the
  Featured section.
- **The server cannot be reached** (no saved network, no Wi-Fi, a timeout):
  aShop opens with the copy of the catalogue on the card, says "Could not reach
  aShop / Showing the apps saved here", and the status bar reads "aShop
  (offline)". Browsing works; downloads need the network.
- **The console was unlinked on the website** (or idle for 180 days): the
  server answers 401, the token is deleted and aShop says "This console was
  unlinked." Link it again from aShop or Settings.
- **Neither the server nor a copy on the card:** "Could not reach aShop" with
  the reason.

## Screens

| Screen | Top | Bottom |
|--------|-----|--------|
| Account required | the welcome screen | why, and Back / Link an account |
| Connecting | the welcome screen, its card naming the step | a spinner |
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

Options > *Check for updates* and *Reload catalogue* both ask the server
again; the first then lists the apps with updates.

## Controls

| Where | Buttons | Touch |
|-------|---------|-------|
| Account required | **A** links an account, **B** or START back to the Home Menu | tap the bar's buttons |
| Main | Left/Right or L/R move along the banners, **A** opens one, **Y** Search, **X** Options, **B** or START back to the Home Menu | swipe the banners, tap a side banner to move to it, tap the middle one to open it, tap the bar's buttons |
| Section | Up/Down pick, Left/Right jump five, **A** opens, **Y** Search, **B** back | tap a row to pick it, tap it again to open it, tap Back / Go! / Search |
| Page | Up/Down (or the Circle Pad) scroll the top screen, **A** downloads, **B** back | tap Back or Download |
| Download | **B** cancels (it stops after the piece coming in); when it ends, **A** or **B** | tap Cancel between pieces, then OK |
| Options | Up/Down and **A**; **B** or **X** closes | tap an item; a tap outside the card closes it |

## Downloads

An app comes in pieces of 6 KB (`GET /v1/store/files/<sha256>.bin` with a
`Range` header), because each reply has to fit the Wi-Fi core's 8 KB reply
buffer with Cloudflare's headers. Each piece is written to
`SD:/Aurora/Apps/<file>.part`, which the Home Menu ignores, and hashed on the
way. When all are in, the SHA-256 is compared with the catalogue's; only a
match is renamed over any old copy, so a cancelled, failed or damaged download
leaves the old app as it was. The version is recorded in `installed.txt`, and
when aShop closes the Home Menu rescans the apps and shows the new one where
the layout file says (new apps at the end of Home), without a restart.

A piece that does not come (no reply, a timeout, a server error) is asked for
again, up to three times, joining the network again first if it dropped.

Joining the network (`wifi_net_join`) mounts the card itself to read
`network.txt` and the firmware, then unmounts it, so its caller must not hold
the card mounted (`include/wifi.h`). aShop unmounts the card for a join and
mounts it again after (`online()` in `Store.c`); during a download the `.part`
file is closed before the join and opened again at its end after it, and the
download stops if it is not as long as the bytes already received.
Reasons on the result screen: no SD card, the connection was lost, the server
did not send the app, the download was damaged, the SD card is full, the
console was unlinked.

The file is asked for by its hash, so a release published during a download
cannot mix two versions.

**Speed.** The Wi-Fi driver reads each received frame a byte at a time (about
0.3 s for a full-size frame, see [`wifi.md`](wifi.md) *Path forward*), so a
download moves at about 4 KB a second: Tetris (28 KB, five pieces) takes about
ten seconds. Faster frame reads in the driver will speed aShop up with no
change here.

While a piece comes in, the core is busy and cannot present frames (see
`wifi_direct_on`), so the download screen's top is drawn in its backbuffer and
copied to the framebuffer on show by the CPU, ten times a second; the floor
keeps moving. B is read straight from the pad then.

## On the SD card

```
SD:/Aurora/Store/
|-- catalog.json    the catalogue as the server last sent it
|-- catalog.tag     its ETag, so an unchanged catalogue is not sent again
|-- icons/          app icons, <first 16 hex digits of the icon's SHA-256>.png
|-- installed.txt   written by aShop: "file version" for each app it put on the card
|-- bag.png         the welcome screen's bag, 92x94
|-- featured.png    banners, 150x108, one per section (named by the catalogue)
|-- news.png
|-- apps.png
|-- games.png
`-- updates.png
```

`Aurora/Store/` in the repository holds the art; copy it to the card with the
rest of `Aurora/`. Without it, aShop draws each banner as a plain tile in the
section's colour with its name, and shows its own icon in place of the bag.
`new3ds.png` (Nintendo's logo) is git-ignored and no longer used.

Banners and icons are decoded once, when aShop opens, at the size they are
drawn (`image_decode_fit`), and a banner's corners are rounded then. PNG and
baseline JPEG both work.

## The catalogue

`GET /v1/store/catalog` answers one JSON document, ASCII only (anything else as
`\u` escapes), fetched in 6 KB pieces when it is larger:

```json
{"v":1,
 "sections":[{"id":"featured","title":"Featured Software","banner":"featured.png",
              "color":"2EC48A","kind":"apps"}, ...],
 "apps":[{"id":"tetris","name":"Tetris","dev":"Aurora","version":"1.1","size":28616,
          "sha256":"cf68...7282","file":"Tetris.bin","icon":"871098e0d17b9498",
          "in":"featured games","text":"The page, with \n line breaks."}],
 "news":[{"id":"1","title":"aShop is open","date":"Oct 2026","text":"..."}]}
```

| Record | Key | Meaning |
|--------|-----|---------|
| section | `id`, `title` | its id (`featured` is where aShop opens) and the name on the title card |
| | `banner` | a picture in `SD:/Aurora/Store` |
| | `color` | `RRGGBB` for the plain tile drawn without a banner |
| | `kind` | `apps` (apps whose `in` names this section), `news` (every news item) or `updates` (every app with an update) |
| app | `id`, `name`, `dev`, `version` | shown on the page and in lists |
| | `size`, `sha256` | the file's length and hash, for the download and its check |
| | `file` | its name in `SD:/Aurora/Apps` |
| | `icon` | the icon's name, also the name of the copy in `icons/`; "" for none |
| | `in` | the sections it is listed in, separated by spaces |
| | `text` | the page: the summary, the description, what is new, cut to about 700 characters |
| news | `title`, `date`, `text` | as for an app |

Text keeps its line breaks, and letters up to U+00FF (what the UI fonts draw)
become UTF-8 (`json_text()` in `src/os/Json.c`); anything else is `?`. An app
whose file name is not a plain name, or whose hash is not 64 hex digits, is
left out. Limits: 8 sections, 64 apps, 16 news items, a 96 KB document, 64 KB
of text in all and 2560 JSON tokens; the server keeps within them.

Every reply carries an `ETag`. aShop sends the one it has as `If-None-Match`
and a `304` means its copy is current. Each piece of a larger catalogue must
carry the same `ETag` as the first; if the catalogue changed meanwhile, aShop
starts again.

## Installed apps and updates

An app counts as installed when its `file` is in `SD:/Aurora/Apps`, which is
what the Home Menu shows too. `installed.txt` remembers the version aShop last
put there, and an installed app has an update when that version differs from
the catalogue's, or when aShop has no record of it (it was copied by hand).
The count is in the status bar, the Download updates section lists them, and
Options > Check for updates asks the server first.

## Publishing apps

Apps are published on the website, at `account.aurora3ds.xyz/developer`:
upload the `.bin` with a 48 x 48 PNG icon, or connect a GitHub repository so
every release with a `.bin` attached becomes a new version. Staff publish at
once; everyone else's releases are checked by staff first. The server's README,
*Publishing on aShop*, has the details.

## Talking to the server

`src/os/Http.c` (`include/http.h`) makes every request, for aShop and for the
account screens: HTTP/1.0 to `3ds.aurora3ds.xyz` on port 80 with `Host`, a
`User-Agent`, `Connection: close`, the console's token as `Authorization:
Bearer`, and extra header lines (`Range`, `If-None-Match`). It looks the host
up once per session and again after a connection fails, and reads the status,
`Content-Length`, `Content-Range` and `ETag` from the reply. One request is one
`WIFI_NETOP_HTTP` (see [`account.md`](account.md)).

| Request | When |
|---------|------|
| `GET /v1/store/catalog` | opening aShop, Check for updates, Reload |
| `GET /v1/store/icons/<hash>.png` | an icon the card does not have |
| `GET /v1/store/files/<sha256>.bin` | a download, piece by piece |

A `401` deletes the token (`account_forget()`) and closes aShop; a `403`
(a token without the `store:read` scope) closes it too.

## Memory

| Address | Size | Use |
|---------|------|-----|
| `0x26E00000` | 2 MB | `STORE_ARENA_ADDR`: decoded banners, the bag and app icons, only while aShop is open |
| `0x25100000` | 2 MB | the picture file being decoded, then the decoder's rows: the image viewer's buffers, idle while aShop is open |
| `0x25700000`, `0x26100000` | | the decoder's own scratch, as for the image viewer |

In the OS's `.bss`: the catalogue (96 KB), its JSON tokens (50 KB), the text
pool (64 KB), one reply (8 KB), an icon (16 KB) and the bar strip kept for
scrolling (43 KB).

## Art

* In the asset pack (`tools/mkassets.py`): aShop's icon (`store.png`) at 64,
  32 and 16 pixels, the auroraShop logo (`icons/store-logo.png`, 191x152, the
  `ART` list), and two masks drawn for aShop in the icon set's style (80x80, a
  7-pixel rounded stroke): `Search.png` and `Wrench.png`.
* The welcome screen's stripes and the download screen's floor are drawn in
  code: the stripes from a 16-row colour table that matches the mock-up, the
  floor as a checkerboard plane in perspective (four samples a pixel, fading in
  toward the horizon), which moves toward the viewer while a download runs.

## Code map

| Part | Where |
|------|-------|
| Catalogue: JSON, the card's copy, records, states | `parse`, `card_catalog`, `card_save`, `records_load`/`records_save`, `states_update` in `src/os/Store.c` |
| Talking to the server | `get_piece`, `fetch`, `sync`, `icons_fetch`, `online`, `token_revoked` in `Store.c`; `http_call` in `src/os/Http.c` |
| Account required | `need_account`, `paint_need` |
| Art | `art_load`, `art_all`, `round_art` |
| Drawing | `stripes`, `checker_floor`, `shade_round`, `buttons`, `tile`, `page_top`, `wrap`, `busy`, `spin` |
| Screens | `main_screen`, `list_screen`, `page_screen`, `download_screen`, `search_screen`, `options_menu`, `notice` |
| Download | `dl_start`, `dl_step`, `dl_finish`, `dl_close`, `dl_tick` |
| SHA-256 in pieces | `sha256_init`/`update`/`final` in `src/os/Nand.c` |

`store_screen(owner)` returns how many apps it installed; `home_open()` in
`src/os/os_main.c` then rescans the apps and calls `home_reload()`
(`src/os/HomeMenu.c`), which re-reads the layout file and keeps the page shown.
The logo preview is `HomeApp.asset_art`.

## What was checked

On the PC, with the real `Store.c`, `Http.c`, `Account.c`, `Json.c`, `Qr.c`,
`screen.c`, `ui.c`, `StatusBar.c` and the image decoders, the card on a folder,
and every request over a real socket to the server under `wrangler dev` (only
the `Host` line changed). Each reply was held to the core's 8 KB less 700 bytes
for Cloudflare's headers; the largest was 6.4 KB. In English, Spanish and
French, all passed:

- not linked: linking from aShop's screen, then the catalogue (206), two icons
  and Tetris in five pieces, byte for byte `Games/Tetris.BIN`, renamed from
  `.part` and recorded;
- opening again: `If-None-Match` and a `304`, no icons fetched; Check for
  updates; Rainbow installed;
- one byte of a download changed on the way: "The download was damaged",
  nothing left on the card;
- a piece lost (timeout): looked the host up again and asked again;
- no network to join: the card's copy, the offline notice and status bar;
- the console revoked on the website: a `401`, the token deleted.

Every frame was looked at (texts fit in all three languages). The SHA-256 in
pieces matches Python's `hashlib` for lengths 0 to 299,999 cut at random.

The folder standing in for the card did not model FatFs mounts, so these
runs missed that the join in "Connecting to aShop..." unmounted aShop's card:
on the console every download then failed with "Could not write to the SD
card", and the catalogue copy, icons and `installed.txt` were not saved
(2026-10-05). Checked since on the real `ff.c` with a RAM disk: the old order
fails as on the console (`FR_NOT_ENABLED`, and `FR_INVALID_OBJECT` for a
join during a download); the new one writes, rejoins twice mid-download,
resumes at the right length and installs the file byte for byte.
Not yet on hardware: speed, and the download screen drawn during pieces.

## Console checklist

1. Deploy the server with aShop (its README, *Deploying*), and publish an app.
2. aShop with no account: "Aurora account required", then Link an account.
3. aShop linked: the catalogue, the icons, the carousel.
4. Download an app; time it; the floor should keep moving; then find it on
   the Home Menu.
5. Close and open aShop again: it should open quickly (no icons fetched).
6. Turn the router off: aShop opens offline with the saved catalogue.
7. Revoke the console on the website: aShop says it was unlinked.
