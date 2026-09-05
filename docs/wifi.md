# Wi-Fi: status, architecture, and technical notes

## Status

The SDIO hardware stack is functional and verified on real hardware (New 3DS).
The Wi-Fi chip is powered, enumerated over SDIO, identified as an Atheros AR6014,
and fully register-accessible via CMD52 and CMD53. No from-scratch driver reaching
this point has been published by the homebrew community.

Everything above the SDIO layer is not yet implemented: the BMI firmware upload and
the HTC, WMI, 802.11, WPA2, DHCP, and TCP/IP layers. The current blocker is BMI. The
chip's bootloader produces no response to command writes (its `HOST_INT_STATUS`
register does not change), which indicates that the exact control-register map and
handshake for this chip must be derived from the NWM sysmodule rather than from the
open-source ath6kl reference. ath6kl matches this chip for enumeration but diverges
at the BMI control layer. Reaching a working `ping` remains a large, multi-stage
effort beyond this point.

## Architecture

- The chip is an Atheros AR6014G-AL1C (DWM-W028 module), an Atheros "thin-MAC" SDIO
  part. It is inert until firmware is streamed into its internal SRAM.
- It is driven from the ARM11. The retail NWM sysmodule (title `0004013000002d02`)
  runs on the ARM11 and accesses the Wi-Fi SDIO controller directly. The ARM9 has no
  SDIO or IRQ path to it in 3DS mode. Aurora's Wi-Fi code therefore runs on the ARM11
  side (the same core as audio and touch) and reports back through a shared FCRAM
  block.
- The full bring-up chain is: chip reset release, SDIO bus init (CMD5/CMD3/CMD7),
  CCCR register I/O (CMD52), enable I/O function 1, BMI firmware upload, undocumented
  handshakes, WMI (READY/REGDOMAIN/BITRATE/SYNC), 802.11 scan and associate, WPA2
  supplicant, and a TCP/IP stack.
- The upper stack is standard Atheros. The NWM `.code` contains the strings `HTC`,
  `WMI CONTROL`, and `WMI DATA BE/BK/VI/VO`, indicating a BMI, HTC, WMI architecture
  identical to the Linux ath6kl driver. ath6kl is therefore a valid protocol
  reference. Its facts can be reimplemented in Aurora's own MIT-licensed code with
  attribution, following the approach already used for `sdmmc.c` (derived from
  GodMode9).

## What works: the SDIO stack

The probe runs on the ARM11 (`src/os/audio11.c: wifi_probe_run()`), triggered from
Settings, Wi-Fi Test. Results are published to the `WifiShared` block at
`0x233B0000` (`include/wifi.h`) and drawn by `wifitest_draw()` in
`src/os/os_main.c`. Each step publishes a `phase` value so that a bad MMIO access
stalls a phase rather than hanging.

The verified bring-up sequence is:

1. Chip reset release: set `GPIO_DATA4_DATA_OUT_WIFI` (`0x10147028`) bit 0 to 1. This
   was the initial blocker. The chip remains in hardware reset until this bit is set,
   so the SDIO interface enumerates but the chip answers nothing.
2. Controller init: a transcription of `sdmmc.c: sdmmc_controller_init()` and
   `set_target()` against the Wi-Fi base (reset pulse, DATACTL, IRQ masks, OPT
   `0x40E9`, 1-bit bus, `setckl(0x20)`).
3. Enumeration: CMD5 (`IO_SEND_OP_COND`, OCR handshake, ready bit 31), then CMD3 (get
   RCA `0x0001`), then CMD7 (select).
4. CCCR and CMD52: read SDIO revision (`0x11`) and card capability (`0x17`), follow
   the CIS pointer, and parse the `CISTPL_MANFID` tuple. Result: manufacturer
   `0x0271` (Atheros), card `0x0201` (AR6014).
5. Enable I/O function 1: write CCCR `0x02` IOE bit 1, poll CCCR `0x03` IOR bit 1 for
   function-core ready. These are the first writes to the chip; `IOR = 0x02` was
   verified.
6. HIF registers: read function-1 `0x400..0x40F` (HOST_INT_STATUS and neighbours),
   which returns varied real data, confirming the ath6kl HIF block base is `0x400`.

## Hardware facts and register reference

| Fact | Value |
|------|-------|
| Wi-Fi SDIO controller base | physical `0x1EC22000` = logical `0x10122000` ("controller 2") |
| Register access | 16-bit TMIO/SDHC, same IP family as `src/sdmmc.c` |
| Chip reset GPIO | `0x10147028` (GPIO_DATA4) bit 0: 0 = reset, 1 = on; no direction register |
| Chip power broker | the MCU over I2C (NWM uses the `mcu::NWM` service) |
| Chip ID | Atheros vendor `0x0271`, device `0x0201` (AR6014), 1 I/O function |
| CCCR SDIO/CCCR revision | `0x11` (1.10 / 1.10) |
| Common CIS | `0x001000` (function 0) |
| HIF register block | function-1 `0x400` HOST_INT_STATUS, `0x401` CPU, `0x402` ERROR, `0x403` COUNTER_INT, `0x404` MBOX_FRAME, `0x405` RX_LOOKAHEAD_VALID, `0x408..` RX_LOOKAHEAD |
| Mailbox 0 (per ath6kl) | function-1 `0x800`; writes end-adjusted to `0x1000 - len` |
| CMD52 argument | bit 31 R/W, bits 30-28 func, bit 27 RAW, bits 25-9 addr, bits 7-0 data |
| CMD53 command word | `0x0035 \| R5 0x400 \| data 0x800 \| (read ? dir 0x1000)`; 16-bit FIFO (`SD_FIFO 0x30`) driven by STAT1 RXRDY/TXRQ |

## What does not work yet: BMI firmware upload

`BMI_GET_TARGET_INFO` (command id 8) is implemented as a first test of the BMI
messaging channel. It requires no firmware, so a valid response would prove the
channel. No response is produced. Confirmed working versus not working:

- CMD53 data phase works. Writing the 4-byte command to the mailbox completes cleanly
  (STAT0 DATAEND set, no errors, no timeouts). The transfer must use the 16-bit FIFO
  path (`SD_FIFO 0x30`, STAT1 RXRDY/TXRQ), not the 32-bit FIFO. The controller signals
  via STAT1 for these small transfers.
- The target never responds. `HOST_INT_STATUS` (`0x400`) stays at `0x10` (bit 4, the
  counter interrupt) regardless of what is written. The mailbox-data-pending bits (low
  nibble) never set, and RX_LOOKAHEAD_VALID stays 0.

Approaches tried against ath6kl's model, none of which produced a response:

- Mailbox write-address end-adjustment (`0x1000 - len`, so the packet ends at the
  mailbox boundary `0xFFF`, which is the AR600x "packet complete" trigger).
- BMI command credit. ath6kl reads a credit counter before each BMI write. The COUNT
  block (`0x420..0x43F`) reads `00 01 FF FF 35 FF FF FF`: counter 1 holds a clean
  `0x01` credit, but its decrement register `0x444` reads `0`, and most of the block
  reads `FF` (unmapped). ath6kl's counter and credit addresses do not line up on this
  chip.

Interpretation: the SDIO transport is solid, but the exact BMI/HIF control-register
map (mailbox address, credit register, and whether the target CPU is running BMI after
this reset sequence) differs from ath6kl's AR6003 map and must be taken from NWM's own
code. That is the next unit of work, and it is reverse-engineering rather than hardware
iteration.

## The extracted firmware

`tools/nwm_extract.py` locates the firmware in the NWM `.code` via the ARM11 literal
pool (marker `0x00524C00`, verify `0x000003ED`, then (end,start) pairs;
`file_off = addr - 0x100000`) and extracts three Main blocks. Sizes match GBATEK:

| block      | size   | note                                   |
|------------|--------|----------------------------------------|
| Main.type1 | 0x1B1B | standard internet firmware (DSi-style) |
| Main.type4 | 0xA5EB | plus "Special AP Mode"                 |
| Main.type5 | 0x7A2E | Special MacFilter/GameID (no internet) |

These blobs are Nintendo's copyright and are kept only in `exclude/` (gitignored).
They are never committed or embedded. When the upload path is implemented, the
firmware must be loaded from the SD card at runtime (for example
`SD:/aurora/wifi/*.bin`) so that users supply their own dump. It must never be baked
into `AURORAOS.BIN`.

## Path forward

1. Reverse-engineer NWM's chip bring-up and control-register map: the exact mailbox
   address, the credit and flow-control mechanism, and any CPU-wake step after reset.
   Landmarks: SDIO command issuer at `0x24940` (calls the SDCMD writer `0x33822`);
   TMIO register helper `0x187dc` (`ctrl==2` selects `0x1EC22000`); firmware-upload
   dispatch `0x28ac0` (selects type1/4/5 by a byte at struct+0xC, computes
   (start,size) from rodata literals: type1 `0x13f664`+0x1B1B, type5 `0x141180`+0x7A2E,
   type4 `0x148bb0`+0xA5EB); descriptor register function `0x1ac20` (table `0x15d168`);
   HIF cluster `0x33800..0x33b50`; upload `0x330ac`, execute `0x33244`.
2. BMI: GET_TARGET_INFO to validate the channel, then WRITE_MEMORY the Main blocks into
   target SRAM, then DONE to start the firmware.
3. HTC service connect, then WMI (READY, REGDOMAIN, BITRATE, SYNC).
4. 802.11 scan, associate, then WPA2 4-way handshake (requires AES, SHA1, PRF).
5. DHCP, then ARP/IP/ICMP for a ping. A full TCP/IP stack for anything more.

## Reverse-engineering setup

rizin 0.9.1 (`/d/rizin-win-installer-vs2019_static-64/bin/rizin.exe`) operates on the
flat `.dec.code`. Load with `-a arm -b 16` (Thumb-2); vaddr equals file offset; runtime
address equals vaddr plus `0x100000`. `aa` finds ~2371 functions; `aac` builds call
xrefs; `axt` lists xrefs to the current seek. Load flat (no `-B`).

## Sources

Reference PDF and driver-plan notes in `exclude/`; GBATEK "3DS Files: Module NWM",
"3DS GPIO Registers", "3DS I2C MCU Register Summary"; 3dbrew NWM_Services, FIRM,
I2C_Registers; nesdev.org thread t=18490; Linux ath6kl driver (AR6003/AR6004, a
functional reference, not register-exact for AR6014).
