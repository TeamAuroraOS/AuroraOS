# Rainbow — a sample Auric app

A tiny [Auric](../auric-lang/README.md) app that fills the top screen with a
ROYGBIV rainbow and returns to the AuroraOS home menu when you press **HOME**.

It demonstrates the three app features:

* **A custom icon** (`rainbow.icon`, a 32×32 nested-arc rainbow) embedded in the
  binary and shown on the home screen.
* **Drawing** with the `fill_rect` built-in (seven colour bands, using named
  colours and raw `0xRRGGBB` values).
* **HOME to exit** — handled automatically by the runtime; the app just keeps a
  `delay()` in its idle loop so HOME stays responsive.

## Files

| File | What |
|------|------|
| `rainbow.aur`  | the program |
| `rainbow.icon` | the 32×32 text icon (`#` = on) embedded via `--icon` |
| `RAINBOW.BIN`  | the built `AUR1` app container |

## Build

From `auric-lang/` (needs devkitARM on `PATH`):

```
python -m compiler.aurc build ../sample/rainbow.aur \
    --icon ../sample/rainbow.icon -o ../sample/RAINBOW.BIN
```

or with the standalone exe:

```
aurc.exe build rainbow.aur --icon rainbow.icon -o RAINBOW.BIN
```

## Run it

Copy `RAINBOW.BIN` to **`SD:\Aurora\Apps\`** on the 3DS SD card, boot Aurora,
and pick **RAINBOW** on the home grid (it shows the rainbow-arc icon). Press
**HOME** to return to the menu.

See [`../docs/apps.md`](../docs/apps.md) for how the home menu discovers apps,
reads their icons, and implements the HOME-return.
