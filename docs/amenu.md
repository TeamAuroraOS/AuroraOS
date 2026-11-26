# The Aurora Menu, the Dock and plugins

| Piece | File |
|-------|------|
| The menu on OS screens (ARM9) | `home_system_menu()` and the `sm_*` functions in `src/os/HomeMenu.c` |
| The menu inside apps (ARM11 core) | `src/os/SystemMenu11.c`; HUDs drawn from `src/os/Gpu11.c` |
| The block both sides share | `AuroraSystemMenuShared` in `include/system_menu.h`, at `0x233A8000` |
| The Dock (pinned apps) | `dock_*` in `src/os/HomeMenu.c`, `SD:/Aurora/pinned.txt` |
| Plugin format, table and API | `include/amenu.h` (OS), `sdk/include/aurora_amenu.h` (plugins) |
| Loading plugins from the card | `src/os/AmenuLoader.c` |
| Building plugins | `sdk/aurcc.py build --amenu`, `sdk/runtime/amenu.ld` |
| Example plugin | `sdk/examples/amenu_demo/main.c` |

**Status:** new in Beta 8 v0.1.7-everest (PR 51 by MooniiCodes, merged
2026-11-23). It builds, and `sdk/tests/test_aurcc.py` builds the example
plugin and checks its header. No console result is recorded here.

The Aurora Menu replaces the old "Aurore Tools" system menu (Return to Home,
Power Off, Cancel). It is a sidebar on the left of the bottom screen with a
Dock of pinned apps on the right, and it can run small plugins from the card.

## Opening it

| Where | How |
|-------|-----|
| Any OS screen (Home Menu, Settings, Files, aShop, terminal, setup wizard) | **L + Down + Select** (`get_keys_down()` in `src/os/os_main.c` calls `home_system_menu()`) |
| The Home Menu | a **double press of the Power button** (two presses within 300 ms); a single press asks "Turn off the console?" |
| An app started from the Home Menu | **L + Down + Select**; the ARM11 core reads the buttons itself, so it opens whatever the app is doing |

The Power button is read from the MCU's interrupt register (I2C register
0x10, bit 0) every 16 passes of the Home Menu's loop (`mcu_power_pressed()`
in `src/os/HomeMenu.c`); only the Home Menu reads it.

The sidebar slides in over eight frames while the screen behind it dims to
about half brightness, and slides out the same way on B.

## The sidebar

| Row | Does |
|-----|------|
| One row per plugin | its name. A plugin with a page opens it with A. A plugin with only a HUD shows ON or OFF and A switches it. A plugin with a page and a running HUD shows "HUD" |
| Home | OS: leaves every screen for the Home Menu (left out before the Home Menu has started). App: Return to Home, as before |
| Brightness | `< n/5 >`; Left and Right change it |
| Reload /amenu | OS only: reads `SD:/Aurora/amenu` again |
| Power Off | asks, then turns the console off |

| Input | Does |
|-------|------|
| Up / Down | choose a row (seven show at a time, the list scrolls) |
| A | the row's action |
| X on a plugin | switches its HUD on or off (plugins with `AMENU_FLAG_HAS_HUD`) |
| Right | moves to the Dock, when something is pinned |
| B | closes the menu |
| Tap a row (OS only) | as A |
| Tap a Dock slot (OS only) | opens that app |

The header says "Aurora" and "MOD: ON". The in-app menu has no touch input.

Brightness from this menu is kept like Settings > Brightness keeps it. On OS
screens the OS takes the level back from the shared block when the menu
closes (`brightness_pull()` in `os_main.c`) and saves `USER.dat` from the
Home Menu's loop, since a screen underneath may be holding the card. Inside
an app the core changes the backlight (`backlight11_set()`) and the shared
block; the OS adopts and saves that level when it starts again after the
app. Every other brightness change in the OS also writes the shared block
(`brightness_to()`), so the menu always starts from the level in use.

Inside an app, Home works only in apps whose runtime set
`AURORA_SYSTEM_MENU_CAP_HOME` (the C SDK from 1.2 and the Auric runtime from
Beta 7). For older apps the menu shows "Built before Beta 7. Press HOME after
closing, or rebuild with the SDK." Power Off asks with a Power Off / Cancel
page in the sidebar.

## The Dock

Up to three apps can be pinned. On the Home Menu, **Y** on an SD app shows a
new item, **Pin to Dock**; on a pinned app it reads **Unpin**, and when three
are pinned it reads **Dock Full (3/3)** and is greyed out. The Dock is saved
in `SD:/Aurora/pinned.txt`, one `name|path` line per app (CRLF), and read at
boot (`dock_load()` in `home_init()`).

The OS copies each pinned app into the shared block (`AuroraDockApp`): its
name, path (up to 127 characters), tint, a 32 x 32 RGB565 icon (the colour
icon, or the 1-bit icon drawn white on the theme's tile gradient) and a
40 x 24 thumbnail of the top screen. The thumbnail is taken the first time
that app's banner is shown on the Home Menu, or when its slot is focused in
the Dock.

| Where the menu is open | Focusing a slot | Choosing it |
|------------------------|-----------------|-------------|
| OS screen | plays the app's 3D banner on the top screen, or shows its name | opens it, as from the Home Menu |
| In an app | the slot lights up | sets `dock_launch_idx` and asks the app to go home; the restarting OS sees the index after `home_init()` and launches that app at once, with `os_ready` set so it gets the OS's fonts and plugin HUDs. Needs `CAP_HOME` like Home. The OS acts on the index only after an app (the core was already running), since on a cold boot that memory holds whatever FCRAM held |

## Plugins

A plugin is a small relocatable binary in `SD:/Aurora/amenu/` with the
extension `.bin` or `.ovl` and the magic `AMNU`. It can add a page to the
sidebar, draw a HUD over both screens, and read and write a running app's
memory or press its buttons.

### Loading

`amenu_load_mounted()` runs inside `scan_apps()`, on the card mount the app
scan already holds: at every OS start (so also after every app) and after
aShop installs or Delete App removes something. Reload /amenu runs it too. It:

1. moves AMNU files it finds in `SD:/Aurora/App` and `SD:/Aurora/App/C` into
   `SD:/Aurora/amenu`. A plugin of the same name already there is kept as
   `<name>.old` (one level; the loader skips `.old`). The app scan does the
   same for AMNU files in `SD:/Aurora/Apps` and `SD:/Aurora/Apps/C`, which are
   not shown as apps;
2. loads up to eight plugins, in directory order, each into its own 120 KB
   slot at `0x27004000 + i * 0x1E000` (to `0x270F4000`);
3. publishes the table (`AmenuTable`, magic `AMTB`, at `0x27001000`) with a
   generation that goes up by one on every load. The cleared table is written
   out to memory before any slot is overwritten, so the core stops running
   HUDs from slots that are being replaced.

A reload resets every slot: HUDs are off again after each return from an app.

A plugin is refused when the magic or ABI version is wrong, the payload is
empty or larger than a slot, the entry is outside the payload, or the
payload and .bss together do not fit. Relocations outside the payload or not
4-byte aligned are skipped.

### File format

All values are little-endian `u32`s.

| Offset | Field | Meaning |
|--------|-------|---------|
| 0 | magic | `AMNU` |
| 4 | abi_version | 1 (`AMENU_ABI_VERSION`) |
| 8 | base_addr | the address it was linked at, `0x27000000` |
| 12 | entry_offset | where `amenu_plugin_entry` is, from the payload's start |
| 16 | payload_size | code, read-only data and data |
| 20 | bss_size | zeroed after the payload |
| 24 | reloc_offset | file offset of the relocation list (`40 + payload_size` from aurcc) |
| 28 | reloc_count | entries in it |
| 32 | flags | `AMENU_FLAG_*` (aurcc writes 0; the flags that count are in the plugin's descriptor) |
| 36 | reserved | 0 |

The payload follows the 40-byte header. Each relocation is the payload
offset of a 32-bit word to which the loader adds `slot address - base_addr`.

### Entry and callbacks

The loader does not call the plugin. The first time either CPU needs a slot
(the OS sidebar, the in-app menu or a HUD tick), it calls
`amenu_plugin_entry(api)`, keeps the returned `AmenuPlugin` if its
`abi_version` is 1, and calls `on_load` once. Whichever side gets there first
passes its own `AmenuApi`, so a plugin must use the `api` each callback
receives rather than keep the one from `on_load` (`aurora_amenu.h` says so).

| Callback | Called |
|----------|--------|
| `on_load(api)` | once, as above |
| `on_menu_open(api)` | when its row is chosen |
| `on_menu_frame(api, held, down)` | while its page is open: every frame on the ARM9, on every change of the buttons or every 250 ms on the ARM11. It draws the page; return 1 to stay, 0 to go back to the list |
| `on_hud_tick(api, app_active, modding_allowed)` | while its HUD is on, on the ARM11 only (see below) |
| `on_toggle(api, enabled)` | when X (or A on a HUD-only plugin) switches the HUD |

`AmenuPlugin` also holds `name` (32), `author` (24), `version` (12) and
`flags`: `AMENU_FLAG_HAS_MENU` (1), `AMENU_FLAG_HAS_HUD` (2) and
`AMENU_FLAG_MODS_APP` (4, not checked by either side). Closing a plugin's
page turns its HUD on when it has the HUD flag.

### HUDs

HUDs run only on the ARM11 core, once the OS has finished booting
(`os_ready`) and while no Aurora Menu is open (the in-app one sets
`menu_open` on the core; the OS sets it for its own sidebar, so a plugin's
page on the ARM9 and its HUD on the ARM11 never run at the same time, and
Reload /amenu can rewrite the slots safely):

* whenever the core presents a frame (`present_one()` and `present_set()` in
  `src/os/Gpu11.c`), `system_menu11_draw_hud()` calls every active HUD after
  the frame is copied into the framebuffer and before it is shown, so
  `hud_rect` and `hud_text` draw into that frame. A call for the top screen
  ignores bottom-screen drawing and the other way round;
* when nothing has been presented for 120 ms, `system_menu11_poll()` calls
  them every 100 ms, drawing straight into the frame on screen.

So a HUD shows on the Home Menu and in apps alike, as long as frames go
through the core.

### The API

`AmenuApi` is a table of functions. The OS (`g_api9` in `HomeMenu.c`) and the
core (`g_api11` in `SystemMenu11.c`) each fill one.

| Group | Functions |
|-------|-----------|
| Sidebar | `sidebar_clear`, `sidebar_header(title, subtitle)`, `sidebar_item(row, label, value, selected)` (rows 0-6), `sidebar_separator`, `sidebar_footer(hint)` |
| Drawing on the sidebar | `draw_pixel`, `draw_rect`, `draw_rect_alpha`, `draw_text` (x from the sidebar's left edge) |
| HUD | `hud_rect(screen, x, y, w, h, rgb, alpha)`, `hud_text(screen, x, y, text, rgb)`; screen 0 top, 1 bottom |
| System | `accent_color`, `now_us`, `keys_held`, `get_brightness`, `set_brightness(0-4)`, `request_home` |
| The running app | `app_active`, `app_modding_allowed`, `app_path`, `app_mem_read`, `app_mem_write`, `app_mem_find`, `app_set_mod_keys` |
| Hardware | `set_accent`, `set_led_color(r, g, b)`, `play_tone(hz)` (0 stops), `get_battery(&charging, &mv, &temp_c)` (returns the percentage), `get_sys_info(&mhz, &is_n3ds, &core_version, &gpu_ops)`, `reboot` |

Colours are `0xRRGGBB`. The sidebar text uses the OS's Figtree fonts: the OS
puts the decoded fonts' addresses in the shared block at boot, and the core
falls back to the 8x8 font when they are not in the asset arena.
`set_led_color` writes the MCU's notification LED pattern (register 0x2D,
`power_set_led()` in `src/power.c`).

Differences between the two sides:

| | OS screens (ARM9) | In an app (ARM11) |
|---|---|---|
| `app_active` | 0 | 1 |
| `app_path` | `/AURORAOS.BIN` | the app's SD path (`app_path` in the shared block, written by `os_launch_info()`) |
| Memory the `app_mem_*` calls accept | `0x22000000-0x23000000` and `0x24000000-0x25000000` (the OS image and the banner/model arena; until 2026-11-26 up to `0x26000000`, which took in the launch and HOME return stubs) | `0x22000000-0x23000000` and `0x24000000-0x25000000` |
| `request_home` | leaves for the Home Menu | Return to Home, if the app has `CAP_HOME` |
| `set_accent` | changes the OS accent until the next start | changes the menu's accent only |
| `now_us` | `timer_ticks() * 15` | `core11_now_us()` |

### Modding a running app

* `app_mem_write` copies the bytes, then (ARM11) bumps `mod_flush_seq`. The C
  SDK runtime's button read (`pad()` in `sdk/runtime/app_hid.c`) flushes its
  caches when the number changes, and cleans its data cache when the menu
  opens and flushes again when it closes, so both sides see the same memory.
* `app_set_mod_keys(mask)` puts buttons in `mod_keys`; the C SDK's `pad()` and
  the OS's `get_keys()` OR them into every button read. The OS clears
  `mod_keys` at boot and at every launch. The Auric runtime does not read it.

**Modding is always allowed.** The C SDK runtime sets
`AURORA_SYSTEM_MENU_CAP_MODDING` next to `CAP_HOME`, but neither side looks at
it: `app_modding_allowed()` returns 1 on both, and the C SDK's own
`app_modding_allowed()` returns true. `aurcc build --no-modding` and
`AURORA_APP_MODDING(0)` are accepted and change nothing (the header always
gets `_app_allow_modding = 1`), and apps built before Beta 8 can be modded
too. This is PR 51's choice (its tests check it); since 2026-11-26 the
comments in `include/system_menu.h`, `include/amenu.h`,
`sdk/include/aurora_amenu.h` and aurcc's `--help` say so instead of
describing a per-app switch. A real switch would mean checking
`CAP_MODDING` in `app_modding_allowed()` on both CPUs.

## Writing a plugin

```
py sdk/aurcc.py new --amenu MyPlugin
py sdk/aurcc.py build --amenu MyPlugin -o MyPlugin.bin
```

Copy `MyPlugin.bin` to `SD:/Aurora/amenu/`, then use Reload /amenu or
restart. `new --amenu` writes a `main.c` with a page and a HUD.

```c
#include <aurora_amenu.h>

static int on_frame(const AmenuApi *api, uint32_t held, uint32_t down) {
  (void)held;
  if (down & AMENU_KEY_B)
    return 0;
  api->sidebar_clear();
  api->sidebar_header("Hello", "");
  api->sidebar_item(0, "Battery", "", 1);
  api->sidebar_footer("B: Back");
  return 1;
}

static const AmenuPlugin g_plugin = {
  AMENU_ABI_VERSION, "Hello", "Me", "1.0", AMENU_FLAG_HAS_MENU,
  NULL, NULL, on_frame, NULL, NULL,
};

AMENU_EXPORT_PLUGIN(g_plugin);
```

How aurcc builds it (`build_amenu()`):

* compiles with `-mcpu=arm946e-s -marm -mno-unaligned-access -ffreestanding
  -fno-builtin`, no C library, linked with `-lgcc` only. The same code runs
  on the ARM9 and on the ARM11 (ARMv6K runs ARMv5TE code), so it must not
  rely on unaligned access;
* links at `0x27000000` with `sdk/runtime/amenu.ld` (`.amenu_entry`, then
  text, rodata and data, then .bss; at most 120 KB in all) and
  `--emit-relocs`;
* collects every `R_ARM_ABS32` and `R_ARM_TARGET1` relocation that lands in
  the payload, and writes the header, the payload (padded to 4 bytes) and the
  list.

Plugins get no `malloc`, no stdio and no file access; everything they reach
goes through `AmenuApi`. `amenu_read32/16/8` and `amenu_write32/16/8` wrap
the memory calls, and `AMENU_APP_RAM_START/END` and
`AMENU_APP_HEAP_START/END` name the ranges.

### The example

`sdk/examples/amenu_demo` ("Aurora ModKit") has a page with two rows,
"Status HUD" (A switches it) and "Back", and a HUD on the top screen. PR 51's
version also had a turbo-A toggle and a memory peek; commit 9f6617c stopped
drawing those rows but left them selectable, and on 2026-11-26 they were
removed with their code.

## Memory

| Address | Size | Use |
|---------|------|-----|
| `0x233A8000` | 12,864 B | `AuroraSystemMenuShared` (a static assert keeps it below `0x233AC000`, the camera block) |
| `0x27001000` | 596 B | `AmenuTable` |
| `0x27004000` | 8 x 120 KB, to `0x270F4000` | plugin slots |

The slots sit between the firm's ARM11 wake mailbox (`0x27000000`) and
`0x27100000`, where the GLB file buffer, the Home Menu's song and a video
app's engine start, so they stay loaded while apps run.

`AuroraSystemMenuShared` keeps its first three cache lines (ARM9 app state,
`menu_open`, `action`), then adds:

| Offset | Field | Written by |
|--------|-------|------------|
| 8, 12 | `accent_rgb`, `brightness` | the OS at boot and launch |
| 96 | `mod_flush_seq`, `mod_keys` (own cache line) | plugins |
| 128 | `app_path[256]` | `os_launch_info()` |
| 384 | the small, regular and bold font addresses, then `os_ready` | `os_main()` |
| 400 | the theme's panel, slot and text colours (six words) | `theme_sync_menu_accent()` |
| 424 | the title font's address | `os_main()` |
| 428 | `dock_count`, `dock_launch_idx`, then `dock[3]` (4,136 bytes each) | the Home Menu |

The core links `include/system_menu.h`, so any change to this block needs an
`AUDIO_CORE_VERSION` bump. It went from 130 to 150 with this release, and to
151 on 2026-11-26 when the camera buffers moved.
