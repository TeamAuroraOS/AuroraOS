# System updates

AuroraOS can update itself over Wi-Fi: **Settings > System Update**, or
`update` in the [terminal](terminal.md). It downloads the newest release from
`update.aurora3ds.xyz`, replaces `AURORAOS.BIN`, `assets.pak` and the firm,
keeps the files it replaced, and restarts into the new AuroraOS.

Nothing here has run on a console yet. The PC tests are listed under
[Testing](#testing).

## Using it

Settings > System Update has these rows:

| Row | Does |
|-----|------|
| Check for Updates | joins the saved Wi-Fi network if needed and asks the server about the newest release |
| Install *Beta 9 v0.1.8* | shown when a newer release exists: asks once, then downloads the updater and starts it |
| Release Notes | the release's notes in the text viewer (the top screen shows the first lines) |
| Restore *Beta 8 v0.1.7-everest* | shown when an update left a backup: puts the replaced files back and restarts |

The bottom of the screen shows the installed version. An update needs 20%
battery, or the charger.

The updater shows six steps on the top screen (battery, network, server,
download, install, start) and each file's state on the bottom one: *already up
to date*, *waiting*, *downloading*, *ready*, *installed*. HOME cancels it while
it downloads, and returns to the unchanged AuroraOS. From *Install the files*
on it must not be stopped, and the screen says *Keep the console on*.

After the restart the new AuroraOS shows *Updated to Beta 9 v0.1.8* once, or
*The update did not finish* with the reason.

In the terminal:

```
update            check, and print the release notes
update install    check, then download the updater and start it
update restore    put back what the last update replaced
```

## How it works

```
 AuroraOS (old)                   update.aurora3ds.xyz             GitHub
 Settings > System Update  --->   GET /v1/latest?current=&build=   releases
   shows notes, battery check <-- {update, name, notes, files, updater}
   downloads Update.bin     ---> GET /v1/files/<sha256>
   checks its SHA-256, writes /Aurora/Update/plan.txt
   starts it like an app
        |
        v
 Updater (updater/, a C SDK app, AUR1)
   asks /v1/latest again, compares each file's SHA-256 with the card's copy
   downloads the ones that differ to /Aurora/Update/new/, checks each
   moves the card's copies to /Aurora/Update/backup/, the new ones in place
   writes /Aurora/Update/result.txt
   puts the new OS where the HOME return stub restores the OS from, exits
        |
        v
 AuroraOS (new)
   deletes Update.bin, plan.txt and new/, shows result.txt once
```

AuroraOS runs entirely from RAM, so it could replace its own files. A separate
updater is used anyway because it is downloaded fresh for every update: the
code that installs a release ships with that release. A later release that
moves files, adds one or converts a settings file only changes the updater,
and consoles on any earlier version install it correctly.

### The server

`update.aurora3ds.xyz` is a Cloudflare Worker in `aurora-site/Update` (its
README says how to deploy it). Every 15 minutes it reads the releases of
`TeamAuroraOS/AuroraOS` on GitHub, pre-releases included and drafts never. It
copies the files a console could be sent into an R2 bucket under their
SHA-256, so a console only ever talks to this one host.

* **Which release.** The newest published release that has an `AURORAOS.BIN`,
  as long as an `Update.bin` exists in it or in an earlier release.
* **Files a release leaves out** are taken from the newest earlier release that
  has them. `assets.pak` only needs attaching when it changed, and so does
  `Update.bin`.
* **Versions.** The console sends its `AURORA_VERSION_NUM` and
  `AURORA_BUILD_DATE`. The server reads each release's version from its
  `AURORAOS.BIN`: the HTTP User-Agent string `AuroraOS/0.1.7-everest` is in the
  binary. A console on that version is up to date whatever the tag says. A
  version no release has (a build between releases) counts as behind when its
  build date is earlier than the newest release's publication date.
* **Notes** are the release's Markdown turned into plain text, at most 6,000
  characters, with typographic quotes and dashes made ASCII for the console's
  fonts.

| Endpoint | Gives |
|----------|-------|
| `GET /v1/latest?current=0.1.7-everest&build=20261123` | `{"update": false, name, version, ...}` or `{"update": true, name, version, published, size, notes, updater: {url, sha256, size}, files: [{name, kind, path, sha256, size, url}]}`; 503 until a release can be installed |
| `GET /v1/files/<sha256>` | a file, with Range requests (the console downloads in pieces) |
| `GET /v1/status` | the synced releases and when GitHub was last read |
| `POST /v1/sync` | reads GitHub now (needs `Authorization: Bearer <SYNC_KEY>`) |
| `GET /` | a page with the newest release, its files and hashes |

`kind` is `os` (to `/AURORAOS.BIN`), `assets` (to `/Aurora/assets.pak`) or
`firm` (to wherever the console's firm is; its `path` is empty). The updater
installs any other kind to its `path` (absolute, no `..`, no drive).

## Publishing a release

1. Set `AURORA_VERSION_NUM` and `AURORA_BUILD_DATE` in `include/aurora.h`. The
   version must differ from the last release's, or consoles on the last one
   are told they are up to date.
2. Build: `make all` (firm), `make os`, `make assets` if the art or sounds
   changed, and `make updater` (`output/Update.bin`).
3. Make the GitHub release (a pre-release is fine) and attach `AURORAOS.BIN`
   and `Aurora.firm`, `assets.pak` when it changed, and `Update.bin` when
   `updater/` changed. **The first release with updates must attach
   `Update.bin`**: until a release has one, the server answers 503.
4. Wait up to 15 minutes, or run
   `curl -X POST -H "Authorization: Bearer <SYNC_KEY>" https://update.aurora3ds.xyz/v1/sync`.
   `https://update.aurora3ds.xyz/` then lists the files the consoles will get.

Consoles on a version from before updates (Beta 8 v0.1.7-everest and older)
have no System Update. They need one update by hand, as before.

## On the SD card

| Path | What |
|------|------|
| `/Aurora/Update/Update.bin` | the updater, while an update runs; deleted at the next start |
| `/Aurora/Update/plan.txt` | from the OS for the updater: `current=`, `build=`, `from=` (the version's name), `firm=` (the firm's path, or empty) |
| `/Aurora/Update/new/` | files being downloaded |
| `/Aurora/Update/backup/` | what the last update replaced (`AURORAOS.BIN`, `assets.pak`, `Aurora.firm`, only those that changed), and `info.txt` (`name=`, `firm=`) |
| `/Aurora/Update/result.txt` | from the updater for the next start: `ok=1` or `0`, `to=`, `why=`, `note=` |
| `/Aurora/Update/notes.txt` | the release notes, for the text viewer |

There is one backup. Each update replaces it, and Restore swaps the backup
with the files in use, so restoring twice goes back to the newer version.

## The firm's path

The firm can be started from `SD:/luma/payloads/` under any name, from
fastboot3DS, or as `boot.firm`. Luma3DS and fastboot3DS pass a FIRM its own
path: `r0` = argc, `r1` = argv, `r2` = `0xBEEF`, argv[0] =
`"sdmc:/luma/payloads/Aurora.firm"`. That string is on the loader's stack,
which is also the firm's stack (both end at `0x08100000`), so `src/start.s`
copies it into `.data` (`g_firm_argv0`) before anything uses the stack. Just
before starting the OS, `src/loader.c` writes it as a FatFs path to
`AURORA_FIRM_INFO_ADDR` (`0x25008400`): the magic `"FWP1"`, then for example
`/luma/payloads/Aurora.firm`. With any other loader there is no magic.

The OS replaces the firm at that path when the file is there, else at
`/luma/payloads/Aurora.firm` when that is there, and otherwise not at all. The
result then says *The firm was not updated: AuroraOS could not tell where it
is.* A firm from before updates writes no path, so the first update's firm
goes to the Luma3DS payload. A firm on NAND (fastboot3DS `nand:`) is never
replaced.

## Safety

* **Every file is checked** against the SHA-256 the server sent, over HTTPS,
  before it is used: the updater by the OS, and each release file by the
  updater, which downloads a damaged file once more before giving up. Files
  already the same as the card's are not downloaded.
* **Battery.** The OS and the updater refuse below 20% unless the charger is
  in, and the updater checks again before it swaps (below 10%).
* **Order.** Nothing on the card changes until every file is downloaded and
  checked. Then `assets.pak` (and any other file) goes first, then
  `AURORAOS.BIN`, then the firm, each by renames on the card. If a rename
  fails, the files already placed are put back and the result says *Nothing was
  changed*. If one cannot be put back, the result says so and the old copies
  stay in the backup folder.
* **The firm's fallback.** If `AURORAOS.BIN` is missing, its header is
  damaged, or it cannot be read, and `Aurora/Update/backup/AURORAOS.BIN`
  exists, the firm's error card offers **Y: Start Backup**. The firm also now
  refuses a header whose payload would land outside FCRAM. This needs the new
  firm, so it protects a console from the first update on.
* **Restore** is in Settings and the terminal, and works without a network.

## Starting the new OS

An app returns to the Home Menu through the return stub the OS installed at
`0x25004000`. The stub copies `desc[1]` bytes from the OS snapshot at
`0x26000000` to `0x22000000` and jumps there. When the updater has installed
everything, it reads the new `AURORAOS.BIN`'s payload into `0x26000000`, sets
`desc[1]` to its size and calls `exit(0)`. The SDK's normal leaving path
(sounds and network stopped, framebuffer A shown) then starts the new OS,
which replaces the ARM11 core if its version differs, as after any app.

This needs an `AOS1` file that loads and starts at `0x22000000` with no ARM11
payload, at most 1 MB (`src/os/os.ld` keeps the OS under that for the
snapshot). If the new OS cannot be started this way, the updater says *Turn the
console off and on to start it* and powers off on A.

Restore starts the restored `AURORAOS.BIN` with `os_boot_system()`
(`os_main.c`): the app launcher's copy-and-jump stub, without the return stub
or the launch block.

## Code

| File | Part |
|------|------|
| `include/update.h` | paths and the OS's functions, shared with the updater |
| `src/os/Update.c` | the check, fetching the updater, restore, the clean-up at start, Settings > System Update |
| `src/os/TermCmds.c` | `update`; AppNet's hooks can be lent to Update.c (`t_appnet_hooks`) |
| `src/os/os_main.c` | the Settings row, `os_boot_system`, `update_at_boot` after the boot |
| `src/start.s`, `src/loader.c` | the firm's path, the backup on the error card |
| `updater/main.c` | the updater (`make updater`) |
| `aurora-site/Update` | the server |

## Testing

* `py updater/tests/run.py` builds the real updater, `src/os/Update.c`, FatFs,
  the JSON reader and BearSSL's SHA-256 for the PC with devkitPro's msys gcc.
  The updater runs against a fake SDK whose SD card is a folder and whose server
  is a folder of files, through these scenarios: an update, a damaged download,
  a failed rename and its rollback, a rename that cannot be undone, up to date,
  low battery (and on the charger), an unknown firm path, a path with `..`, no
  return stub, and no plan. The OS side runs on a FAT32 RAM disk: restore
  there and back, the firm's path, the clean-up and its message, and
  `update_check` / `update_fetch` against the server's reply format. Breaking
  the rollback or the hash check makes them fail.
* `npm test` in `aurora-site/Update` (16 tests): the sync with a fake GitHub,
  the versions, the files with Range, the HTTPS redirect. The sync was also
  run with `wrangler dev` against the real GitHub releases.

## Limits

* One backup, of the last update only.
* Files of other kinds are installed but not backed up by name for Restore.
* The firm is replaced only on the SD card, and only where it was found.
* The screens are in English only.
* An update downloads the whole of each changed file; there are no patches.
