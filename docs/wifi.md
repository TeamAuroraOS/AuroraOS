# Wi-Fi — status, architecture, and the realistic path

**Status: not working, and not close.** Wi-Fi on the 3DS is an unsolved problem
for bare-metal / from-scratch code — no one in the homebrew scene has published
a working from-scratch driver. This file records what was learned, the one
concrete step that is done, and the staged path if we choose to pursue it.

## Architecture (corrected from the reference PDF, checked vs 3dbrew/GBATEK)

- The Wi-Fi chip is an **Atheros AR6014G-AL1C** (DWM-W028 module on O3DS/2DS/XL),
  an Atheros "thin-MAC" SDIO part. It is **inert until firmware is uploaded**.
- It is driven **from the ARM11**, not the ARM9. The retail **NWM** sysmodule
  (title `0004013000002d02`) runs on ARM11 and pokes the Wi-Fi **SDIO** controller
  registers directly. The ARM9 has no SDIO/IRQ path to it in 3DS mode. So any
  Aurora Wi-Fi code must live on the **ARM11** side (same place as our audio/touch
  core).
- Full bring-up chain: power/clock gate (SDMMCCTL — *bit still unconfirmed by the
  RE community*) → SDIO bus init (CMD5/CMD3/CMD7, 4-bit, then CMD52/53 register
  I/O) → **BMI** firmware upload (the Atheros Xtensa firmware) → a few
  undocumented handshakes → **WMI** protocol (READY/REGDOMAIN/BITRATE/FRAMERATES/
  SYNCHRONIZE) → 802.11 scan/associate → **WPA2 supplicant** → a **TCP/IP stack**
  before anything is actually usable.

## What is done

The Atheros firmware is embedded in the NWM `.code` file. `tools/nwm_extract.py`
locates it via the ARM11 literal pool GBATEK documents (marker `0x00524C00`,
verify `0x000003ED`, then (end,start) address pairs; `file_off = addr - 0x100000`)
and extracts the three compressed **Main** firmware blocks. Sizes match GBATEK
exactly for our dump:

| block       | size    | note                                   |
|-------------|---------|----------------------------------------|
| Main.type1  | 0x1B1B  | standard internet firmware (DSi-style) |
| Main.type4  | 0xA5EB  | + "Special AP Mode"                    |
| Main.type5  | 0x7A2E  | Special MacFilter/GameID (no internet) |

These blobs are **Nintendo's copyright** — kept only in `exclude/` (gitignored),
never committed or embedded. A user must dump/extract them from their own console.
(The small Stub.data/Stub.code/Database blocks are also in the `.code`, just before
Main.type1; not extracted yet.)

## Why this is not an "iterate on hardware" task like audio/touch

Audio and touch worked because they were **documented register pokes** we could
correct against a reference driver and verify quickly. Wi-Fi is different on every
axis:
- The key registers (power gate, the Wi-Fi SDIO controller base) are **provisional
  / unconfirmed** even in the RE community's own notes.
- The BMI/WMI handshake and firmware format require **disassembling** the 348 KB
  ARM11 NWM binary (Ghidra/IDA), not just reading a datasheet.
- Even after the link is up, "Wi-Fi working" needs an 802.11 state machine, a WPA2
  handshake, and a TCP/IP stack — each a substantial subsystem, none done
  bare-metal by anyone publicly.

Realistically this is a multi-month reverse-engineering project, not a build task.

## Static-analysis findings (Phases 1-2 of the dev plan)

Done here by scripting against the dump (no Ghidra needed for these):

- **NWM never references the SD/SDIO controller bases directly.** `0x10006000`,
  `0x10007000`, `0x10100000` appear **zero** times in the `.code` as 32-bit pool
  literals, and are never assembled via `MOVW`/`MOVT` either. So the wiki's loose
  "SDIO controller 3 @ 0x10100000" is NOT the register block NWM actually pokes.

- **Phase 2 (register ownership) — answered from the ExHeader, authoritatively.**
  NWM's ARM11 kernel capabilities grant it direct MMIO to exactly one range:
  a map-memory-range pair **physical 0x1EC22000 – 0x1EE22000** (a 2 MB ARM11 IO
  window; on 3DS the ARM11 IO physical base `0x1EC00000` mirrors the logical
  `0x10100000` IO region, so this window is logical ~`0x10122000`-`0x10322000`).
  It is **not** granted `0x10006000`/`0x10007000` (the ARM9-side SD controllers)
  nor `0x10100000` itself. Conclusion: **NWM drives the Wi-Fi hardware directly
  from the ARM11** (not IPC-proxied to the ARM9 for the bus), within that window
  — so an Aurora Wi-Fi driver belongs on the **ARM11** side, and the SD path
  (0x10006000, our existing sdmmc.c) is confirmed *not* the Wi-Fi path.

- **What still needs Ghidra:** the exact SDIO-controller offset inside that window,
  the power/clock-enable sequence, and the BMI/WMI packet layouts. NWM accesses
  the window through a kernel-assigned mapped virtual address, so those offsets
  only fall out of disassembling the actual load/store instructions (Phase 3).

Note: NWM's `.code` contains the **BMI/WMI protocol + firmware-upload** logic
(the pool at file `0x28B40` anchors the upload routine), but the low-level SDIO
transport + power-gating sequence is thin here — expect to source the SDIO/power
bring-up from Aurora's own SD driver + the nesdev SDMMCCTL work, and the BMI/WMI
layer from this binary.

## Staged plan (if pursued)

1. **Find the Wi-Fi SDIO controller + power gate** — RE the NWM `.code` (its early
   init) + the nesdev thread's SDMMCCTL work; confirm the controller base and the
   power/clock bit against a live dump. (Blocking: needs disassembly + hardware.)
2. **ARM11 SDIO bring-up** — CMD5/CMD3/CMD7 + 4-bit; then CMD52/CMD53 to read the
   Atheros chip ID. First real milestone: *chip detected on the bus.*
3. **BMI firmware upload** — port the BMI command sequence from the disassembly;
   upload the extracted Main.typeN blocks to target SRAM.
4. **WMI bring-up** — READY_EVENT onward; scan for APs.
5. **Associate + WPA2 + DHCP + TCP/IP** — the long tail.

Sources: reference PDF in `exclude/`, GBATEK "3DS Files — Module NWM", 3dbrew
NWM_Services / FIRM, nesdev.org thread t=18490, Linux ath6kl (loose analogy only).
