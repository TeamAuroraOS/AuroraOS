# Wi-Fi: status, architecture, and technical notes

## Status

**Wi-Fi works (2026-10-03, New 3DS).** AuroraOS joins open and WPA2-PSK
networks from Settings > Wi-Fi, gets an address over DHCP, and the terminal's
`ping` reaches names on the internet. On core v112 a WPA2/WPA3 home router
took the first RSN element form (capabilities `0x0000`), both keys went in,
DHCP gave an address and the router answered 4 pings of 4 in 48 ms.

**The firmware copies itself (2026-10-03, not yet run on a console).**
When the firmware is missing from the SD card, Settings > Wi-Fi offers to copy
it from the console's own NWM module, read from the system NAND after a button
code, so no PC is needed (see *Copying the firmware on the console*).

**Core v113 drops the debug log.** With everything working, the
`SD:/Aurora/WiFi_Log.txt` writer (`WiFiLog.c`), the Wi-Fi Test's full test
(X), the core's command trace at `0x27D80000` and the values only the log
read (the EEPROM copy, the assert register dump, the firmware checksum, the
controller register snapshot) were removed; a join no longer spends about
0.4 s reading the EEPROM copy back. The logs quoted below are from before.
The removed code was never committed; a copy is in `exclude/wifi-log-removed/`.

**Core v112 (2026-10-03) joins WPA2 networks.**
WPA2-PSK with AES (CCMP) is done on the console: the ARM9 makes the PMK from
the password, the core runs the EAPOL-Key 4-way handshake and group rekeys,
and installs the keys in the firmware (see *WPA2-PSK*). In Settings > Wi-Fi a
secured network now asks for its password and connects; WPA3-only, enterprise
(802.1X), WEP and old WPA networks are tagged and refused with a reason. The
crypto passes the standards' test vectors, and the handshake passes a PC test
against an access point built from the standard, checked again with Python's
own `hashlib`. Two things only hardware can tell: which RSN element the
firmware puts in its association request (three forms are tried, see below),
and whether it takes the keys as sent.

**Core v111 ping works on hardware (2026-10-03,
`WiFi_Log_With_Ping.txt`):** with the beacon reports off, the router answers
in about 48 ms (from 0.9 s), and the terminal's `ping` works.

**Core v110 joined a network on hardware (2026-10-03, `WiFi_Log_5.txt`).**
From Settings > Wi-Fi the New 3DS joined an open phone hotspot on its first
`WMI_CONNECT` (pinned to the BSSID, 2442 MHz, -23 dBm), got `10.48.199.94`
over DHCP on the second DISCOVER, learnt the router's MAC with ARP and had 3
of 4 pings to it answered. The replies took 0.9 to 1 s: the BSS filter the
scan sets (every beacon) was still on, so each of the network's ten beacons a
second came up as a 277-byte `BSSINFO`, and reading one a byte at a time
takes about 90 ms, so a reply queued behind several of them.

**Core v111 (2026-10-03, not yet run on hardware)** turns the filter off
(`NONE_BSS_FILTER`) as soon as the join succeeds, keeps the join up when asked
to (`WIFI_OPT_STAY`), and adds `AUDIO_CMD_WIFI_NET`: DNS look-ups and pings to
any address over that link. The terminal's `ping` uses it (see *Staying
joined: the terminal's network*), and Settings > Wi-Fi joins now stay up.

**Core v110 (2026-10-03) joins open networks.**
Settings > Wi-Fi searches, lists the networks with signal and lock, and
joins an open one when it is picked: the core scans, sends `WMI_CONNECT` the
way Octoblimp's port found to work, then asks for an address over DHCP, finds
the router with ARP and pings it four times, and leaves (see *Joining a
network*). A secured network's password can be typed and is saved, but
joining one needs WPA2, which is not written yet. Each join writes
`WiFi_Log.txt`, and the full test (X) adds a join of the saved network as its
fifth run. The IP stack and the join logic pass host tests; whether the
firmware takes our data frames is the open question.

As of core v109 (2026-10-03). **The firmware runs, talks WMI and scans.**
On core v108 three scans in a row completed (`SCAN_COMPLETE`, status 0) with
no firmware assert, and the best listed six networks with channel, signal,
BSSID, security and name. On core v105 the New 3DS booted the NWM firmware, connected the WMI
control service and received `WMI_READY` with the console's MAC address
(`04:03:D6:06:AC:CF`, which the console's own settings confirm, and which the
firmware's EEPROM copy holds too, checksum valid), firmware version
`0x230000EB`. On core v107 its scan reported a real access point (a secured
network on channel 3, -68 dBm) as a `BSSINFO` event.

**The assert during the scan (v107) is understood.** About 57 ms after
`START_SCAN` the v107 firmware raised counter 0, its debug (assert) interrupt,
and went quiet. That build connected only the WMI control service and scanned
actively. On v108, connecting the four WMI data services (BE, BK, VI, VO), as
NWM, ath6kl and Octoblimp's port all do, stopped it; so did a passive scan
without them. An active scan sends probe requests, and the firmware evidently
needs the data endpoints set up to transmit.

**Events on the data endpoints.** The v108 log also showed the firmware
sending a `BSSINFO` event (a 575-byte probe response) on data endpoint 2, behind
NWM's two-byte data prefix (RSSI, then info `0x80`: bits 7:6 = 2, the type
ath6kl calls ACL and drops). Core v109 takes such frames as WMI events, which
is why its active scans should report more networks.

Also settled by the v107 log: the `stub_data` re-write after the LZ stream is
not needed (preset 16 reaches WMI too); restoring the SoC registers before
`BMI_DONE` breaks the boot like a missing patch does (preset 1, so
`LOCAL_SCRATCH` bit 3 has to stay set); and `Main.type1` with the database
dies at once after `BMI_DONE` (the patch is relocated for `Main.type4`).

**The v106 crash.** The test screen built each line in a 48-byte buffer, and
WifiShared holds whatever FCRAM held until a core writes it; after a power-on
those values printed as long digit strings that ran past the buffer and over
the saved registers (the crash dump showed ASCII digits in R6, R7 and R11). The
buffer now fits the longest line any values make, `draw_char` stops at the
right edge of its buffer instead of writing into the next one, and the core
zeroes WifiShared when it starts.

**What made the difference: the firmware patch.** Only preset 0 got past
HTC_READY. With every other preset the chip stopped answering every command,
even CCCR reads, within microseconds of the host reading HTC_READY: the firmware
failed and the chip reset. Preset 0 is the only one that, after the main image,
writes `DATABASE.BIN` to `0x53FE18` and points `hi_dset_list_head` (HI + 0x18)
at it. That blob is an AR6K DataSet list with a BDIFF patch stream for the
firmware, relocated for exactly that address; Octoblimp's port found that
without the registration the firmware "ran unpatched and asserted internally".
The Linux port's finish, which the other presets copied, skips it. Preset 0 is
now the default, and the full test also runs it without the `stub_data`
rewrite (16) and with the SoC registers restored (1), to see whether either
matters.

**The first log found a compiler bug behind the flaky uploads.** The ARM11
runs with SCTLR.U clear (legacy alignment), where an unaligned halfword or word
access silently goes to the aligned address. GCC, building for ARMv6, assumed
unaligned accesses work: in `wifi_bmi_write_mem()` it merged two byte stores of
the length field into a halfword store at `sp+9`, which landed on `sp+8`, so
byte 10 of every `BMI_WRITE_MEMORY` header kept whatever the stack held. The
v104 log shows it directly: the first stub write went out with length
`0x006E0038` instead of `0x38` (and `0x00300038` on the retry), the chip waited
for megabytes that never came and granted no further credit. Whether an upload
worked depended on one stale stack byte, which any code change could move. That
fits every "stuck in STUB" run from v89 to v95, so the causes recorded for those
(DATA32 writes, credit polling, sleep) are unproven; v96 worked with the byte at
zero. The same build also read the `WMI_READY` MAC address and firmware version
with unaligned word loads, which would have scrambled them. The ARM11 is now
built with `-mno-unaligned-access` (core v105), and no unaligned access remains
in the core.

The log's timestamps also show that `wifi_ms(n)`, a counted loop on an uncached
core, takes 6 to 8 times `n` ms: the 3 s BMI waits are about 20 s, the 10 ms HTC
poll is about 70 ms and the 2 s HTC windows about 15 s. They were left alone
while the alignment fix was tested by itself; that test passed (v105), so
making them real is on the *Path forward*.

Working on hardware (New 3DS): the chip is powered and reset, enumerated over
SDIO, identified as an Atheros AR6014, register-accessible through CMD52 and
CMD53, and the diagnostic window reads and writes its memory. BMI works, the
NWM firmware uploads (383 sends with preset 0, no missed credit) and starts:
it sets `hi_board_data_initialized` (HI + 0x58), posts `HTC_MSG_READY` (10
credits of 1544 bytes), accepts `HTC_CONNECT_SERVICE` for WMI control
(endpoint 1, messages up to 1538 bytes) and `HTC_SETUP_COMPLETE`, and sends
`WMI_READY`. The whole handshake takes about 0.8 s after `BMI_DONE`.

### How it got here

- **v84 (2026-09-26): first HTC_READY.** Three changes from the Linux 3DS port
  went in together (see *A working reference*): powering the subsystem through
  `CFG11_WIFICNT`, the nocash finish sequence, and `Main.type1`. Type4 reached
  HTC_READY too on the next build, so the image was not the fix.
- **v85 to v88:** the connect-service message went out and the target never
  replied (see *The HTC handshake*).
- **v89 to v95: the upload regressed**, lately stalling in the STUB phase.
  Three causes were found:
  1. v89 wrote the DATA32 block registers (`0x104`/`0x108`) for every CMD53.
     Byte mode, which every BMI send uses, must not touch them; since v93 they
     are written in block mode only.
  2. `COUNT_DEC` (`0x450`) takes a credit on every access. v91 polled it more
     often and v94 read it as four separate bytes (ath6kl's 4-byte read is a
     single access), and both took credits the target had just granted.
  3. nocash's finish clears `SYSTEM_SLEEP` (`0x40C4`) bit 0 to "restore" it,
     but the ROM already runs with sleep disabled (`0x1D`), so it switched
     sleep on. On v95 the chip stopped answering the command right after that
     write (send 375 of 378, CMDTIMEOUT). The earlier "stuck at s N" runs
     were the ARM11 hung inside a single controller access.
- **v96: the upload fixed.** It follows the Linux 3DS port and ath6kl more
  closely:
  - The credit counter is read as one byte per poll, a millisecond apart, for
    up to 3 s (the Linux port's BMI timeout for this chip).
  - No send without a credit, ever (ath6kl's rule). A missing credit, a
    refused transfer or a reply that never comes ends the attempt, and the
    attempt is redone from a cold start: subsystem power off under reset, then
    on.
  - The chip is kept awake at the finish (`0x40C4` bit 0 stays set). Preset
    11 restores nocash's value, which the v84-v88 boots used.
  - A reply is only read once the lookahead says it is there; whatever a reply
    leaves in the mailbox is drained and counted (`x`), and the target-info
    reply is read by its own byte count, so no stale word can shift a later
    reply.
  - A data transfer that fails resets the controller before the next command.
  - 100 ms before the first BMI command (the Linux port's `msleep(100)`).
  - `hb` is the controller access in progress, published before every one, so
    a hang shows where it stopped.

  On hardware it completed the upload on the first attempt and HTC_READY came
  back; the connect write was then refused, as described above.

## Architecture

- The chip is an Atheros AR6014G-AL1C (DWM-W028 module), an Atheros "thin-MAC" SDIO
  part. It is inert until firmware is streamed into its internal SRAM.
- It is driven from the ARM11. The retail NWM sysmodule (title `0004013000002d02`)
  runs on the ARM11 and accesses the Wi-Fi SDIO controller directly. The ARM9 has no
  SDIO or IRQ path to it in 3DS mode. Aurora's Wi-Fi code therefore runs on the ARM11
  side (the same core as audio and touch) and reports back through a shared FCRAM
  block.
- The full bring-up chain is: subsystem power and chip reset, SDIO bus init
  (CMD5/CMD3/CMD7), CCCR register I/O (CMD52), enable I/O function 1, BMI firmware
  upload, the HTC handshake, WMI (READY/REGDOMAIN/BITRATE/SYNC), 802.11 scan and
  associate, WPA2 supplicant, and a TCP/IP stack.
- The upper stack is standard Atheros. The NWM `.code` contains the strings `HTC`,
  `WMI CONTROL`, and `WMI DATA BE/BK/VI/VO`, indicating a BMI, HTC, WMI architecture
  identical to the Linux ath6kl driver. ath6kl is therefore a valid protocol
  reference, and its facts are reimplemented in Aurora's GPL-2.0 Wi-Fi code with
  attribution (see *License and credits*).

## What works: the SDIO stack

The probe runs on the ARM11 (`src/os/WiFi11.c: wifi_probe_run()`), triggered from
Settings, Wi-Fi Test. Results are published to the `WifiShared` block at
`0x233B0000` (`include/wifi.h`) and drawn by `wifitest_draw()` in
`src/os/os_main.c`. Each step publishes a `phase` value so that a bad MMIO access
stalls a phase rather than hanging.

The verified bring-up sequence is:

1. Power: set `CFG11_WIFICNT` (`0x10140180`) bit 0. The chip enumerates and
   answers BMI without it, which is why it went unnoticed for so long.
2. Chip reset: pulse `GPIO_DATA4` (`0x10147028`) bit 0 low, then high. The chip
   stays in hardware reset while the bit is 0, and a full pulse is needed to put
   it back in BMI; it also stops answering once the ARM9 has used the SD
   controller.
3. Controller init: a transcription of `sdmmc.c: sdmmc_controller_init()` and
   `set_target()` against the Wi-Fi base (reset pulse, DATACTL, IRQ masks, OPT
   `0x40E9`, 1-bit bus, `setckl(0x20)`).
4. Enumeration: CMD5 (`IO_SEND_OP_COND`, OCR handshake, ready bit 31), then CMD3 (get
   RCA `0x0001`), then CMD7 (select).
5. CCCR and CMD52: read SDIO revision (`0x11`) and card capability (`0x17`), follow
   the CIS pointer, and parse the `CISTPL_MANFID` tuple. Result: manufacturer
   `0x0271` (Atheros), card `0x0201` (AR6014).
6. Enable I/O function 1: write CCCR `0x02` IOE bit 1, poll CCCR `0x03` IOR bit 1 for
   function-core ready (`IOR = 0x02` verified), set the function 1 block size to 128
   and enable the card interrupt (CCCR `0x04`).
7. HIF registers: read function-1 `0x400..0x40F` (HOST_INT_STATUS and neighbours),
   which returns varied real data, confirming the ath6kl HIF block base is `0x400`.

## Hardware facts and register reference

| Fact | Value |
|------|-------|
| Wi-Fi SDIO controller base | physical `0x1EC22000` = logical `0x10122000` ("controller 2") |
| Register access | 16-bit TMIO/SDHC, same IP family as `src/sdmmc.c` |
| Subsystem power | `CFG11_WIFICNT` (`0x10140180`, u8) bit 0 |
| Chip reset GPIO | `0x10147028` (GPIO_DATA4) bit 0: 0 = reset, 1 = on; no direction register |
| Chip power broker | the MCU over I2C (NWM uses the `mcu::NWM` service) |
| Chip ID | Atheros vendor `0x0271`, device `0x0201` (AR6014), 1 I/O function |
| CCCR SDIO/CCCR revision | `0x11` (1.10 / 1.10) |
| Common CIS | `0x001000` (function 0) |
| Function 1 block size | 128, set through FBR `0x110`/`0x111` before any mailbox access |
| HIF register block | function-1 `0x400` HOST_INT_STATUS, `0x401` CPU, `0x402` ERROR, `0x403` COUNTER_INT, `0x404` MBOX_FRAME, `0x405` RX_LOOKAHEAD_VALID, `0x408..` RX_LOOKAHEAD |
| Error bits (`0x402`) | `0x01` wakeup, `0x02` RX underflow, `0x04` TX overflow; write the bits back to clear |
| Interrupt enables | `0x418..0x41B` (INT_STATUS_ENABLE); ath6kl zeroes all four around the handshake |
| BMI credit | `COUNT_DEC[4]` at `0x450` (`0x440 + 4 * 4`); every read takes a credit |
| Mailbox 0 | function-1 `0x800..0xFFF`; a write ending at `0xFFF` (EOM) completes a message, so sends go to `0x1000 - len` |
| Diagnostic window | data `0x474`, write address `0x478`, read address `0x47C`; writing the address LSB starts the access |
| CMD52 argument | bit 31 R/W, bits 30-28 func, bit 27 RAW, bits 25-9 addr, bits 7-0 data |
| CMD53 command word | `0x0035 \| R5 0x400 \| data 0x800 \| (read ? dir 0x1000)`; 16-bit FIFO (`SD_FIFO 0x30`) driven by STAT1 RXRDY/TXRQ |
| CMD53 modes | byte mode for BMI (DATA16 block length only); block mode adds arg bit 27, the DATA32 pair `0x104`/`0x108` and, as the Linux driver does, the SDIO-command bit `0x4000` |

## A working reference: the Linux 3DS port

In August 2026 a Linux port got this chip working on the same hardware, which
makes it the first end-to-end reference for the layer Aurora is stuck at:

* [`Peugeot205GTI/linux-3DS` PR 3](https://github.com/Peugeot205GTI/linux-3DS/pull/3),
  "Add AR6014 WiFi support": an ath6kl driver that knows the AR6014, plus fixes
  to the 3DS SDHC driver and its SDIO interrupt handling.
* [`Peugeot205GTI/arm9linuxfw` PR 2](https://github.com/Peugeot205GTI/arm9linuxfw/pull/2),
  "Turn on the AR6014 WiFi Card": the ARM9-side power-on.

Both are GPL-2.0, as is Aurora's Wi-Fi code. Three differences from what Aurora
was doing came out of reading them:

**1. The Wi-Fi subsystem has a power register we never touched.** The ARM9
firmware sets `CFG11_WIFICNT` (`0x10140180`) bit 0 as well as clearing
`SDMMCCTL` (`0x10000020`) bit 2, the Wi-Fi SDIO power-off bit. Aurora only
released the reset GPIO at `0x10147028`. Bit 2 is already clear here, because
the SD driver writes `0x340`, but `CFG11_WIFICNT` was never set. `wifi_bringup()`
now sets it before the reset pulse, and the Wi-Fi Test screen shows the value as
`wc`.

**2. The main firmware image.** The Linux container LZ-streams `Main.type1`,
the plain internet firmware. Aurora's boot was written against the ath6kl 3DS
port, which uses `Main.type4` (internet plus Special AP Mode). **L** boots
type1 and **R** type4 on the test screen.

**3. The finish sequence.** nocash's `sdio_bmi_finish`, which the Linux port
follows, restores the two SOC registers the upload changed
(`0x40c4` sleep, `0x180c0` scratch) before `BMI_DONE`, and writes nothing else
to the host-interest area except `HI + 0x6c` = 0x80 and `HI + 0x74` = 0x63.
Aurora's sequence left both registers modified, and after the LZ stream it
re-wrote `stub_data` and the database and set `HI + 0x18`. That turned out to be
the part that matters, the other way round: on hardware only the sequence that
writes the database and sets `HI + 0x18` gets past HTC_READY (see *Status*).
The `stub_data` re-write lands at `0x524C00`, the address the LZ stream just
decompressed the main firmware to, and the boots with it work; preset 16 tests
whether it is needed at all.

These are switchable at runtime, because each test costs a hardware run. The
`WIFI_OPT_*` bits in `include/wifi.h` ride in the command block: bit 0 restores
the SOC registers, bit 1 skips the writes after the LZ stream, bit 2 leaves
the interrupt enables alone, bit 3 lets the chip sleep again at the finish, and
bit 4 skips only the `stub_data` re-write. **Y** on the Wi-Fi Test screen cycles
the preset, shown as `o`:

| `o` | Means |
|-----|-------|
| 0 | the default: after the LZ stream, `stub_data`, the database and `HI + 0x18`; every HTC service; active scan |
| 64 | preset 0 with a passive scan |
| 32 | preset 0 connecting WMI control alone (the v105-v107 behaviour) |
| 16 | preset 0 without the `stub_data` re-write (reaches WMI too) |

Bits 5 and 6 (`WIFI_OPT_CTRL_ONLY`, `WIFI_OPT_PASSIVE`) act after the boot.
On hardware, presets 1 (SoC registers restored), 3, 7 and 11 (the Linux finish
without the database) all stop right after HTC_READY and are no longer
offered.

## The HTC handshake

Once BMI hands over, mailbox 0 carries HTC frames: a 6-byte header (endpoint,
flags, payload length, two control bytes) then the payload. A frame from the
target can carry a trailer of credit and lookahead records, counted inside the
payload length, with its size in the first control byte.

Every transfer is padded to the 128-byte block size in both directions: the
target only releases a frame once a whole block has moved. Reading just the
frame (v85) left the rest of the block in the mailbox, so the next read came
back stale and the write after it overflowed the target's FIFO. Sends are
end-aligned like BMI commands; reads come back a byte at a time with CMD52.

The handshake is four messages on endpoint 0, all in ath6kl's legacy layout,
which is what this firmware speaks:

| Step | Message | Payload |
|------|---------|---------|
| 1 | HTC_READY, from the target | id 1, credit count, credit size, endpoint count |
| 2 | HTC_CONNECT_SERVICE | id 2, service id `0x0100` (WMI control), flags, meta length |
| 3 | HTC_CONNECT_SERVICE_RESPONSE | id 3, service id, status, endpoint, max message size |
| 4 | HTC_SETUP_COMPLETE | id 4 |

After step 4 the firmware starts WMI and sends `WMI_READY` (`0x1001`) on the
endpoint it just assigned. AR6014 uses the short WMI header, a bare 2-byte
event id with no interface field, and a 12-byte ready event: MAC address, PHY
capability, a reserved byte, and the firmware version.

With the firmware patch registered (preset 0), the first write style, ath6kl's
CMD53 byte mode, is answered: CONNECT_SERVICE_RESPONSE gives endpoint 1, and
WMI_READY follows SETUP_COMPLETE. Before that, every way of writing step 2
failed, because the firmware without its patch had already failed and reset
the chip: the "refused" CMD53s were CMDTIMEOUTs on every command, CCCR reads
included. The write styles below were the search for a transport problem that
was not there; they stay, tried in order, in case a firmware variant needs
another:

| Style | Transfer |
|-------|----------|
| 0 | CMD53 byte mode, one padded block: what ath6kl ends up doing, since the mmc core only uses block mode for more than one block |
| 1 | CMD53 block mode, one block |
| 2 | CMD52, a byte at a time |
| 3 | the bus switched to 4-bit (CCCR 0x07, then the controller), then byte mode |
| 4 | 4-bit, block mode |

## WMI and the scan

NWM's firmware puts a bare u16 id in front of every WMI command and event,
without the `info1` field of ath6kl's `WMI_CMD_HDR` (Octoblimp's port, from
NWM's disassembly; WMI_READY's layout confirms it). Commands go to the WMI
endpoint (1) in one HTC frame each, padded to a block like the control
messages. Each takes one of the 10 credits HTC_READY granted (ath6kl's legacy
HTC gives them all to the service endpoints; endpoint 0's control messages do
not use them), and credit reports in the trailers of incoming frames give them
back. With two or fewer left a frame asks for a report
(`HTC_FLAGS_NEED_CREDIT_UPDATE`). A command is not sent without a credit. On
v107 no credit report came back during a scan.

Before `SETUP_COMPLETE`, core v108 connects the four WMI data services too
(`0x0101`-`0x0104`, best effort, background, video, voice) with ath6kl's
connection flags `0x0005` (reduce credit dribble, threshold one half).

After WMI_READY, the core runs the discovery scan Octoblimp's port found to
work on this firmware, all values theirs:

| Command | Id | Payload |
|---------|----|---------|
| SET_POWER_MODE | 18 | MAX_PERF (2): the firmware's default power save misses frames |
| SET_PROBED_SSID | 10 | slot 1, any SSID |
| SET_SCAN_PARAMS | 8 | periods 0xFFFF, dwell times 0 (target default), short-scan ratio 3, flags 0x2F, 3 probes per SSID |
| SET_CHANNEL_PARAMS | 17 | 802.11g, the 13 channels 2412-2472 MHz (channel 14 is 802.11b only and makes the command fail) |
| SET_BSS_FILTER | 9 | every beacon |
| START_SCAN | 7 | long scan, the same 13 channels listed explicitly (an empty list scans for 13-17 s) |

Results come as `BSSINFO` events (`0x1004`) in NWM's full 16-byte header
(channel in MHz, frame type, SNR, RSSI, BSSID, IE mask), then the beacon or
probe-response body; the SSID is its first information element. The scan ends
with `SCAN_COMPLETE` (`0x100A`). A rejected command comes back as `CMDERROR`
(`0x1005`: command id, error code). Up to 16 networks are kept in
`WifiShared.bss`; frames are now read up to 1664 bytes (a 1538-byte message,
padded). The firmware also sends `REGDOMAIN` (`0x1006`) right after
WMI_READY; on this console its value is `0x80000188`.

**Firmware asserts.** Counter 0 in `COUNTER_INT_STATUS` (`0x403` bit 0) is the
target's debug interrupt (ath6kl's `ATH6KL_TARGET_DEBUG_INTR_MASK`): the
firmware asserted and will not answer WMI again. Every HTC poll checks it; once
it is set the waits end, and Settings > Wi-Fi says the firmware stopped. (The
register dump `hi_failure_state` (HI + 0x04) points to, 60 words through
`AR6002_VTOP`, is what ath6kl's `ar6000_dump_target_assert_info` reads.)

## Joining a network

A boot with `WIFI_OPT_CONNECT` (`0x80`) goes on, after the scan, to join the
network in the request block `WifiReq` at `0x233B8000` (`include/wifi.h`):
magic `WFRQ`, the SSID, and for a password the PMK made from it and a seed
for the SNonce. The ARM9 writes it (`wifi_request()` in `src/os/WiFi9.c`)
before posting the boot and wipes it when the core is done
(`wifi_request_clear()`). The password itself never leaves the ARM9, and the
PMK never reaches `WifiShared`.

**Picking the BSS.** While the scan runs, every `BSSINFO` whose SSID matches is
compared, and the strongest is kept (BSSID, channel, signal, capability, and
its security from the RSN element), even when the 16-entry list is full. If
none was seen, the core scans once more. Each network's security is in
`WifiBss.sec`: open, WEP (privacy and no RSN or WPA element), WPA (the old
WPA element only), WPA2 (RSN with PSK and CCMP), WPA3 (SAE without PSK),
802.1X (enterprise), or TKIP (PSK without CCMP). Only open and WPA2 are
joined; anything else stops with `WIFI_CONN_SECURED` and the class as its
detail, as does a WPA2 network with no password given (detail `0xFF`).

**Connecting.** Octoblimp's port found that a plain `WMI_CONNECT` gets
`NO_NETWORK_AVAIL` (disconnect reason 1) from this firmware, and what works is
this order:

| Command | Id | Payload |
|---------|----|---------|
| SET_PROBED_SSID | 10 | slot 0, `SPECIFIC_SSID_FLAG` (1), the SSID |
| SET_SCAN_PARAMS | 8 | the stock values: periods 0, dwell 0, ratio 3, flags 0x2F, 3 probes |
| SET_CHANNEL_PARAMS | 17 | 802.11g, one channel: the network's |
| SET_BSS_FILTER | 9 | every beacon |
| CONNECT | 1 | 52 bytes: infrastructure, open auth, no WPA, no ciphers (`NONE` is 1 in the AR6014 enums), the SSID, channel in MHz, the BSSID, and `ctrl_flags` `CONNECT_PROFILE_MATCH_DONE` (0x08) |

The core waits up to 10 s for `CONNECT` (`0x1002`: channel, BSSID, intervals,
network type) or `DISCONNECT` (`0x1003`: 802.11 status, BSSID, the firmware's
reason). It tries twice like that, then once unpinned (no BSSID, all 13
channels, no flags, the plain connect), each retry after a `WMI_DISCONNECT` to
stop the firmware's own attempt. Our own `DISCONNECT` comes back with reason 3
and is not taken as a failure.

**Data frames.** Out, on the best-effort endpoint, as ath6kl's legacy driver
sends them: NWM's two-byte data prefix (`00 00`: no RSSI, 802.3 data, priority
0), then the frame as 802.3 with an LLC/SNAP header (`AA AA 03 00 00 00` and
the Ethernet type), the length field counting from the LLC header. Each frame
takes a credit from the same pool as WMI commands; the target's credit reports
refill it. In, a frame on a data endpoint whose prefix has data type 0
(info bits 7:6) is 802.3; it is turned back into Ethernet II and handed to the
IP stack. Type 2 frames are WMI events, as before.

**The IP stack** (`src/os/Net11.c`, from the RFCs) is the least that proves
the link: DHCP (DISCOVER, OFFER, REQUEST, ACK; broadcast flag set, client id,
host name `AuroraOS`), ARP (asking for the router, learning its MAC from any
ARP it sends, answering for our own address) and ICMP echo (pinging the router,
answering pings). DHCP is tried four times, 4 s each; ARP three times; then
four pings, 2 s each. The round trip includes the 10 ms polling interval and
reading the reply a byte at a time, so it is an upper bound. At the end the
core sends `WMI_DISCONNECT`, so the access point does not keep a station
nobody serves, unless the boot asked to stay (next section).

**Timing.** Joining polls on the private timer, 10 ms a poll, instead of
`wifi_ms`. A join adds about 5 s to the 18 s a boot and scan take when
everything answers, and about 100 s at worst.

The results go to `WifiShared` (`conn_stage` and the fields after it):
`WIFI_CONN_*` for how far it got, the BSS, the last disconnect reason, the
address, mask, router, DNS, lease, the router's MAC, and the pings.

## WPA2-PSK

The console does what wpa_supplicant does for ath6kl: the firmware associates
with the RSN element it builds from the connect command, and the host runs
the EAPOL-Key exchanges over the data path and hands the keys back. The Linux
3DS port gives the AR6014's numbers: auth mode `WPA2_PSK` is 5, cipher `AES`
4 (`TKIP` 3, `NONE` 1), and its `WMI_ADD_CIPHER_KEY` (22) has no MAC address
field: index, cipher, usage (0 pairwise, 1 group), length, 8-byte RSC, the
key in 32 bytes, and the key-op control (`KEY_OP_INIT_VAL`, 3), 45 bytes.

| Step | Where |
|------|-------|
| PMK = PBKDF2-HMAC-SHA1(password, SSID, 4096, 32), or the 64 hex digits themselves | ARM9, `wifi_request()`; about 0.2 s with the caches on |
| `WMI_CONNECT`: auth 5, pairwise AES, group AES or TKIP as the network has it | core, `wifi_join_try()` |
| message 1 -> PTK = PRF-384(PMK, "Pairwise key expansion", addresses and nonces) -> message 2 with our RSN element and the MIC | core, `src/os/Wpa11.c` |
| message 3: ANonce, replay counter and MIC checked, the GTK unwrapped with the KEK (AES key wrap) -> message 4 | `Wpa11.c` |
| 100 ms for message 4 to leave unencrypted, then the pairwise key (TK) and the group key | core, `wifi_wpa_keys()` |
| group message 1 (a rekey) -> new GTK -> group message 2; installed at the next command | `Wpa11.c`, `wifi_net_op()` |

`src/os/Crypto.c` has SHA-1, HMAC, PBKDF2, the 802.11 PRF and AES-128 with
key wrap, built for both CPUs; `src/os/Wpa11.c` is the station side of the
exchanges (key descriptor version 2 only, which a CCMP pairwise cipher
gives). EAPOL frames (type `0x888E`) are taken from the 802.3 data path
before the IP stack sees them.

**The RSN element.** Message 2 has to repeat, byte for byte, the RSN element
the firmware put in its association request, or the access point drops the
station. The CONNECT event does not report the request (this firmware sends
the association response in its place), so the core builds the element: PSK,
CCMP pairwise and the network's group cipher, with RSN capabilities `0x0000`
first, then `0x000C` (16 PTKSA replay counters), then none. An access point
that takes message 2's MIC but not its element drops the station at once
(hostapd: reason 2); that moves to the next form. `WifiShared.wpa_variant`
says which form worked; on hardware the first one did.

**A wrong password.** The access point cannot check message 2's MIC, so it
sends message 1 again and gives up after a few tries (reason 15, 4-way
handshake timeout). Two message 1s, or that reason, with no message 3 in 8 s,
stop the join with `WIFI_CONN_BADPASS`; Settings says "Wrong password?".

**The SNonce** comes from a seed the ARM9 hashes from its timer around the
PBKDF2 work, mixed with the core's timer when the handshake starts: unique per
join, though not from a hardware random source.

## Staying joined: the terminal's network

With `WIFI_OPT_STAY` (`0x100`) a join that got an address and the router's
MAC does not leave at the end: the core keeps the link as a session
(`WifiShared.session` = 1) and returns, free for audio and the GPU again. The
firmware keeps the association by itself; nothing reads the chip until the
next command, so frames queue up in it meanwhile. A new boot of any kind ends
the session (the chip is reset).

`AUDIO_CMD_WIFI_NET` (12) runs one operation on the session, `arg0` one of
`WIFI_NETOP_*`, with its operands and results in `WifiNetIo` at `0x233B8100`
(`include/wifi.h`). Each first reads what queued up since the last command
(64 frames at most); a `DISCONNECT` among them ends the session and the
answer is `WIFI_NETS_NOLINK`.

| Op | Does |
|----|------|
| `STATUS` | only that: whether the link is still up |
| `DNS` | an A query for `name` to the DNS server DHCP gave (the router if none), up to three tries; `ip`, or `NONAME` (NXDOMAIN), `DNSFAIL`, `BADNAME`, `TIMEOUT` |
| `PING` | one echo to `ip` with `seq`: 56 data bytes as Linux sends; `rtt_us`, `ttl`, `bytes`, `from`, or `ICMPERR` with the type and code of an unreachable or time-exceeded that quotes our echo |
| `LEAVE` | `WMI_DISCONNECT`, and the session ends |

A frame for an address off our subnet goes to the router's MAC; one on the
subnet first needs that host's MAC, asked for with ARP (one entry is kept).
Each wait is `timeout_ms` (100 to 10000, 2000 by default).

On the ARM9, `src/os/WiFi9.c` posts the command and waits for it with the
screens drawn directly (`wifi_net()`), and `wifi_net_join()` in `os_main.c`
joins the network saved in Settings > Wi-Fi with `CONNECT | STAY`. The
terminal's `ping` (docs/terminal.md)
joins first when there is no session, shows the seconds while it does, then
resolves the name and sends one echo a second. The status bar's Wi-Fi icon is
white while a session is up (`wifi_online()`).

## Settings > Wi-Fi

The console-side setup. The Settings row shows the saved network's name.

- **Search for networks** boots the chip and scans (about 20 s). The list has
  one row per name, strongest first, with signal bars and a lock on secured
  networks; hidden networks are left out. The saved network stays listed
  even when the search did not see it.
- **Picking an open network** saves it and joins it (about half a minute),
  and the console stays joined for the terminal. The top screen then says
  `Connected to ...` with the address, the router and the pings, or why it stopped: not found (the 3DS only sees 2.4 GHz, channels 1
  to 13), the firmware's disconnect reason, no DHCP answer, and so on.
- **Picking a secured network** opens the keyboard in its password mode,
  filled in with the saved password when it is the saved network: the `abc`
  key (or L) cycles lower case, upper case and a layer with every ASCII
  symbol; B deletes, START is done, SELECT cancels. A password of 8 to 63
  characters (or a 64-hex-digit PSK) is saved with the network and the
  console joins it. A wrong one ends with "Wrong password?".
- Networks AuroraOS cannot join show their security as a tag (`WPA3`,
  `802.1X`, `WEP`, `WPA`, `TKIP`) and say why when picked.
- **Y** on the saved network forgets it.
- Without the firmware on the SD card it first offers to copy it from the
  console (see the next section).

The network is kept in `SD:/Aurora/wifi/network.txt`, two lines,
`ssid=` and `password=`, in plain text. While a command runs the core cannot
present frames, so the progress screen (what the chip is doing, a spinner,
the seconds) is drawn straight into the framebuffer on screen, with CPU
drawing only. B twice stops waiting; the screen then stays on a "still
finishing" page until the core is done, and B there leaves Wi-Fi settings.

## Copying the firmware on the console

When Settings > Wi-Fi opens and `SD:/Aurora/wifi` lacks any of the five files
the driver loads, it says so and asks **"Wi-Fi firmware required: Copy it from
this console?"**. Yes moves to a warning, **"This action accesses the system
NAND. Proceed?"**, with a code of five buttons drawn at random from Up, Down,
Left, Right, A and B (never the same twice in a row). The code has to be
pressed in order; a wrong button turns the chips red and starts it over, and
SELECT, START or the Cancel button leave without touching the NAND. Then the
copy runs, about a second, with its five steps on the top screen, and ends on
"Wi-Fi firmware saved" or on the step that failed and why. The files are the
six `tools/nwm_extract.py` writes, byte for byte the same.

What it does (`src/os/FwDump.c`, `src/os/Nand.c`, `src/os/Aes.c`):

1. **Read the system NAND.** The eMMC is brought up for reading with
   GodMode9's `Nand_Init()` sequence (`sdmmc_nand_init`), and sector 0, the
   unencrypted NCSD header, gives CTRNAND: the partition with file system type
   1 and crypt type 2 (Old 3DS) or 3 (New 3DS). CTRNAND is AES-CTR encrypted
   with keyslot 0x04 or 0x05. The bootrom leaves both keyXs and the 0x04 keyY;
   the New 3DS's 0x05 keyY is the value its firmware sets (Luma3DS's
   `crypto.c` sets the same value; it matches GodMode9's SHA-256 check for
   it). The counter is the first 16 bytes of SHA-256 of the eMMC's CID plus
   the byte offset divided by 16. Before anything else reads it, CTRNAND's
   first sector must decrypt to an MBR, so a wrong key stops here. CTRNAND is
   then FatFs drive `1:`, through a hook in `src/diskio.c`.
2. **Find the Wi-Fi module.** The newest `*.app` in
   `1:/title/00040130/00002d02/content` (then `20002d02`) whose NCCH program
   ID is NWM's.
3. **Decrypt the module.** The exheader and the ExeFS are decrypted with
   keyslot 0x2C (keyX from the bootrom, keyY the first 16 bytes of the NCCH
   signature) and the NCCH counter (version 0 and 2: the partition ID
   big-endian and the section type; version 1: the partition ID and the
   section's offset). Only this original NCCH crypto is handled: a module with
   a fixed key, a seed or a 7.x-and-later keyslot is reported, since those
   keys are not on the ARM9.
4. **Extract the firmware.** The ExeFS `.code` is unpacked (the 3DS's
   backwards LZ, when exheader flag bit 0 says so) and the blobs are found the
   way `tools/nwm_extract.py` finds them: the literal pool marked
   `0x00524C00`, `0x000003ED`.
5. **Save it to the SD card.** Six files in `SD:/Aurora/wifi`; if one cannot
   be written, all six are removed again.

**Nothing writes the NAND.** The SD/MMC driver has no eMMC write at all, and
FatFs drive 1 refuses writes (`RES_WRPRT`) and reports itself write-protected.
The NCCH keyslot's keyY is changed, as any NCCH load does.

The copy uses the image viewer's buffers (`0x25100000` for the `.code`,
`0x25700000` for it unpacked), idle while Settings is open.

Checked on the PC: SHA-256 against the standard vectors; `Nand.c` against
fake NANDs for both crypt types, encrypted with a stand-in for the AES engine
(partition table, keyslot, counter offsets, aligned and unaligned reads, a
damaged MBR, no NCSD); and the whole flow in the host harness with the NWM
module this console's GodMode9 dump came from, unencrypted and encrypted both
ways: the six files matched the PC extractor's exactly, and a fixed-key, a
7.x and a missing module each stopped with their reason. The eMMC init and
the AES engine's register use follow GodMode9 and Luma3DS but have not run on
a console yet.

## The Wi-Fi Test screen

Settings, Wi-Fi Test. **A** (or a tap) runs the probe, **R** boots `Main.type4`,
**L** boots `Main.type1`, **Y** cycles the boot preset, **B** leaves. A boot takes several
seconds; B also cuts the wait short.

| Line | Fields |
|------|--------|
| `core vN ph P clk C t/o T` | ARM11 core version, probe phase (4 = done), controller clock register, commands that timed out |
| `Cnn arg resp s0/s1` | the logged SDIO commands, green when answered; four rows once a boot has run |
| `tr S/N f C T @A:N x E/B` | boot step (3 HI, 4 STUB, 5 MAIN, 6 DONE) and sends completed, updated on every send; the first failed command (`0434` a CMD52, `0053` a CMD53 data write, `0405` a BMI reply that never came), its STAT0 \| STAT1 << 16 (bit `0x0040` of the top half is CMDTIMEOUT: the chip did not answer), the attempt and send count it failed at; reply words drained / the target-info byte count (12 expected) |
| `hb H sl S sc C` | the controller access in progress: step << 28 (1 waiting to send a command, 2 waiting for its response, 3 done; 4-8 the same for a data command, 6 moving data through the FIFO, 7 waiting for the transfer to end) \| register << 16 \| command word; `SYSTEM_SLEEP` and `LOCAL_SCRATCH` as read from the ROM |
| `drop n/id cc C t n/e` | frames drained before the connect and the last one's id; the card's CCCR after boot as bytes, FN1 block size low byte, bus width (0 = 1-bit, 2 = 4-bit), I/O ready, I/O enable (enumeration leaves `80000202`); write styles tried and a bit per style the controller refused |
| `s a b c d` | the four CMD53 styles (byte, block, 4-bit byte, 4-bit block): host int \| error << 8 \| lookahead valid << 16 \| MBOX_FRAME << 24 after the write, or, with bit 31 set, the controller's STAT1 STAT0 when it refused the write (`0040` in STAT1 is CMDTIMEOUT, `0002` CRC, `0008` data timeout; none of them with no DATAEND means the card held the line busy) |
| `scan S N nets NAME` | in place of `s` once WMI is up: the scan stage (setup, scanning, done, timeout), networks found and the strongest one's name, or `err` and the last rejected command |
| `join S ADDRESS n/m` | in place of `scan` after a join: how far it got (`notfound`, `secured`, `failed` with `rN` the disconnect reason, `nodhcp`, `noping`, `done`), the address DHCP gave, and pings answered of sent |
| `ATHEROS 0271:0201 fn1 RDY wc W o O` | chip ID from the CIS, function 1 ready, `CFG11_WIFICNT` after the write, boot preset |
| `FW Tn STEP rdy R s N nc C [b 2] pl P` | image type, boot step reached, ready flag, mailbox sends, credit waits that ran out (each ends the attempt), `b 2` when the boot was redone from a cold start, credit polls made (climbing = the ARM11 is alive) |
| `HTC id I cr C sz S [ep E]` | HTC_READY: message id, credits, credit size, and the endpoint once connected |
| `r R mb M` | when no HTC_READY came: the interrupt state, and the stub readback (`0x21006136` when the stub landed) or a peek of mailbox 0 |
| `WMI 1001 MAC ...` or `HTC ep E st S sg G r R` | the MAC from WMI_READY; otherwise the connect endpoint, status (255 = nothing came back), how far the handshake got (1 READY to 5 WMI) and the interrupt state |

In probe mode the lower lines show the diagnostic-window reads (`diag`) and the
BMI target version and type (`BMI ver 0x2300006F ty 0x00000002`) instead.

## Rules for changing the driver

Each of these was learned from a regression on hardware:

- The ARM11 must be built with `-mno-unaligned-access` (Makefile
  `ARM11_CFLAGS`): its alignment mode turns an unaligned access into one at the
  aligned address, silently. After touching ARM11 code, the core can be checked
  with `objdump -d build/audio11.elf` for `ldrh`/`strh` at odd offsets or
  `ldr`/`str` at offsets that are not a multiple of 4.
- While the core runs a Wi-Fi command it cannot present frames. The Wi-Fi Test
  and Settings > Wi-Fi draw straight into framebuffer A until the command
  finishes, and never post a command while another is still running. Drawing
  then has to be CPU only: `ui_wallpaper` copies with the GPU, so a progress
  screen uses `ui_wallpaper_rect`.
- The Wi-Fi password stays on the ARM9; the PMK made from it stays in
  `WifiReq` (wiped after the command) and in the core's handshake state
  (wiped when the join ends). Neither may be copied into `WifiShared`, which
  the screens read.
- The BMI upload is proven with byte-mode CMD53 exactly as v88 sent it. New
  transfer behaviour goes behind the `blksz` parameter of `wifi_cmd53()`, never
  into the shared path.
- The credit counter is read one byte at a time, one read per poll: every
  access to `0x450` (or its neighbours) takes a credit.
- Never send without a credit, and never read a reply the lookahead has not
  announced; abort the attempt and redo it instead.
- Every wait must end quickly when the chip goes silent. The ARM11 also polls
  touch and serves audio, so a long spin freezes the whole console.

## Historical: the BMI wall

The notes below are the state of the BMI channel as first written. BMI has
worked since core v39, when the mailbox moved to CMD52, and the firmware first
answered with HTC_READY on core v84. They are kept because the reasoning is what
found the mailbox and credit mechanism.

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

Interpretation at the time: the SDIO transport is solid, but the exact BMI/HIF
control-register map (mailbox address, credit register, and whether the target CPU
is running BMI after this reset sequence) differs from ath6kl's AR6003 map and must
be taken from NWM's own code.

## The extracted firmware

`tools/nwm_extract.py` locates the firmware in the NWM `.code` via the ARM11 literal
pool (marker `0x00524C00`, verify `0x000003ED`, then (end,start) pairs;
`file_off = addr - 0x100000`) and extracts six blobs: `stub_data`, `stub_code`,
`database` and three Main images. The Main sizes match GBATEK:

| block      | size   | note                                   |
|------------|--------|----------------------------------------|
| Main.type1 | 0x1B1B | standard internet firmware (DSi-style) |
| Main.type4 | 0xA5EB | plus "Special AP Mode"                 |
| Main.type5 | 0x7A2E | Special MacFilter/GameID (no internet) |

These blobs are Nintendo's copyright. They are never committed or embedded:
each console copies its own, either on the console (Settings > Wi-Fi offers it
when they are missing; see *Copying the firmware on the console*) or on a PC
from a GodMode9 dump of NWM with this extractor. Either way `STUBDATA.BIN`,
`STUBCODE.BIN`, `DATABASE.BIN`, `MAINTYP1.BIN` and `MAINTYP4.BIN` end up in
`SD:/Aurora/wifi/`, and Settings > Wi-Fi and the Wi-Fi Test load them from
there into shared FCRAM before each boot. Main.type5 is copied but not used.

## Path forward

1. Servicing the chip between commands (today frames wait in it until the
   next one), then UDP and TCP sockets on top of `Net11.c` for anything more
   than ping.
2. WPA3 (SAE) would need the firmware to pass authentication frames to the
   host, which this one may not do; WPA2/WPA3 transition networks already
   work as WPA2.
3. Make the boot's delays real (`wifi_ms` runs 6-8 times long, see *Status*;
   the join already uses the timer), and move frames to CMD53 reads, the
   4-bit bus and a faster clock: today every byte of a frame is read with its
   own CMD52 at about 523 kHz, about 0.3 s for a full-size frame.

## Reverse-engineering setup

rizin 0.9.1 (`/d/rizin-win-installer-vs2019_static-64/bin/rizin.exe`) operates on the
flat `.dec.code`. Load with `-a arm -b 16` (Thumb-2); vaddr equals file offset; runtime
address equals vaddr plus `0x100000`. `aa` finds ~2371 functions; `aac` builds call
xrefs; `axt` lists xrefs to the current seek. Load flat (no `-B`).

NWM landmarks: SDIO command issuer at `0x24940` (calls the SDCMD writer `0x33822`);
TMIO register helper `0x187dc` (`ctrl==2` selects `0x1EC22000`); firmware-upload
dispatch `0x28ac0` (selects type1/4/5 by a byte at struct+0xC, computes
(start,size) from rodata literals: type1 `0x13f664`+0x1B1B, type5 `0x141180`+0x7A2E,
type4 `0x148bb0`+0xA5EB); descriptor register function `0x1ac20` (table `0x15d168`);
HIF cluster `0x33800..0x33b50`; upload `0x330ac`, execute `0x33244`.

## License and credits

The Wi-Fi driver code (the SDIO/BMI/HIF bring-up in `src/os/WiFi11.c`, plus
`src/os/Net11.c`, `src/os/net11.h`, `src/os/Wpa11.c`, `src/os/wpa11.h`,
`src/os/Crypto.c`, `include/crypto.h`, `src/os/WiFi9.c`, the
Wi-Fi parts of `src/os/os_main.c` (the Wi-Fi Test and Settings > Wi-Fi),
and `include/wifi.h`) is
licensed **GPL-2.0**, separately from the rest of AuroraOS. It derives register
facts and the BMI/HIF bring-up sequence from the **ath6kl legacy driver**
(GPL-2.0) as ported to the Nintendo 3DS by **Octoblimp**. Because GPL-2.0 is
copyleft, any binary that links this Wi-Fi code is covered by GPL-2.0.

Specific facts sourced from Octoblimp's ath6kl 3DS port: the BMI command-credit
register `COUNT_DEC + (HTC_MAILBOX_NUM_MAX + ENDPOINT1)*4 = 0x450`; mailbox 0 at
`0x800` (byte-increment, no end-address adjustment); `RX_LOOKAHEAD_VALID` at
`0x405`; the function-1 SDIO block size of 128; and the firmware target-address
map (`main_type1` -> `0x524C00`, `database` -> `0x53FE18`, host interest
`0x00500400`). Also from that port: `DATABASE.BIN` as a DataSet list and BDIFF
patch that the firmware applies once `hi_dset_list_head` points at it; NWM's
u16 WMI command and event header; the full 16-byte `BSSINFO` header; and the
discovery-scan sequence and values (power mode, probed SSID, scan parameters,
802.11g channel table, BSS filter, explicit channel list); and the connect
sequence (the SSID in probe slot 0, stock scan parameters, a one-channel
table, a connect pinned to the BSSID with `CONNECT_PROFILE_MATCH_DONE`), the
AR6014's auth and cipher enum values, and the data frames' `00 00` prefix.
`src/os/Net11.c` follows RFC 2131/2132 (DHCP), RFC 826 (ARP), RFC 1035 (DNS)
and RFC 792 (ICMP); `src/os/Wpa11.c` IEEE 802.11-2016 12.7 and 802.1X-2004;
`src/os/Crypto.c` FIPS 180-4, RFC 2104, RFC 8018, FIPS 197 and RFC 3394.

Facts sourced from the Linux 3DS port (GPL-2.0, techflashYT and Peugeot205GTI;
see *A working reference*): `CFG11_WIFICNT` (`0x10140180`) bit 0 as the Wi-Fi
subsystem power, `SDMMCCTL` bit 2 as the Wi-Fi SDIO power-off bit, `Main.type1`
as the image their container streams, the finish sequence that restores
`0x40c4` and `0x180c0` before `BMI_DONE`, the DATA32 block registers and the
SDIO-command bit of their SDHC driver, and ath6kl's HTC timing and interrupt
handling; and the AR6014's auth-mode and cipher numbers and its
`WMI_ADD_CIPHER_KEY` layout without a MAC address. That port in turn follows nocash's wifiboot, documented in GBATEK.

The firmware copy (`src/os/FwDump.c`, `src/os/Nand.c`, `src/os/Aes.c`) is not
part of the GPL-2.0 driver: it is GPL-3.0 like the rest of AuroraOS. Its eMMC
bring-up is GodMode9's `Nand_Init()`, as is the rest of `src/sdmmc.c`; the AES
engine's register sequences and the New 3DS CTRNAND keyY follow Luma3DS's
`crypto.c` (GPL-3.0); the code unpacking follows ctrtool's `lzss.c`; the NCSD,
NCCH and counter layouts are from 3dbrew.

## Sources

Octoblimp's ath6kl legacy 3DS port (GPL-2.0); reference PDF and driver-plan notes
in `exclude/`; GBATEK "3DS Files: Module NWM", "3DS GPIO Registers", "3DS I2C MCU
Register Summary", "MBOX Transfer Headers"; 3dbrew NWM_Services, FIRM,
I2C_Registers; nesdev.org thread t=18490; Linux ath6kl driver (`htc_mbox.c`,
`hif.c`, `sdio.c`); the Linux 3DS port's AR6014 support (`Peugeot205GTI/linux-3DS`
PR 3, `Peugeot205GTI/arm9linuxfw` PR 2), which follows nocash's wifiboot.
