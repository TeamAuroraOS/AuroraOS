# Wi-Fi: status, architecture, and technical notes

## Status

As of core v97 (2026-09-30). **Work is paused here.** v97 is built but has not
been run on hardware; *Path forward* says where to pick it up.

Working on hardware (New 3DS): the chip is powered and reset, enumerated over
SDIO, identified as an Atheros AR6014, register-accessible through CMD52 and
CMD53, and the diagnostic window reads and writes its memory. BMI works, the
NWM firmware uploads (378 of 378 sends on the first attempt, no missed credit)
and starts: the firmware sets `hi_board_data_initialized` (HI + 0x58) to 1,
which the host never writes, and posts `HTC_MSG_READY` on mailbox 0 (message
id 1, 10 credits of 1544 bytes, the standard ath6kl credit size).

**Where it stops: the HTC connect.** After boot, every CMD53 write of the
connect-service frame is refused by the controller (`t 5/15`, four timeouts),
and the CMD52 fallback is accepted but never answered. The refused transfer is
byte for byte the kind that had just succeeded 378 times during the upload, so
the card's SDIO side changes once the firmware runs. v97 records the
controller's status for each refused style, reads the card's CCCR after boot
(`cc`), and tries the one bus difference left against the Linux port: it runs
the firmware on a 4-bit bus.

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
re-wrote `stub_data` and the database and set `HI + 0x18`. The `stub_data`
re-write is the suspicious one: it lands at `0x524C00`, the same address the LZ
stream just decompressed the main firmware to, so it overwrites the first 0x38
bytes of the image the chip is about to run.

These are switchable at runtime, because each test costs a hardware run. The
`WIFI_OPT_*` bits in `include/wifi.h` ride in the command block: bit 0 restores
the SOC registers, bit 1 skips the writes after the LZ stream, bit 2 leaves
the interrupt enables alone, and bit 3 lets the chip sleep again at the finish.
**X** on the Wi-Fi Test screen cycles the preset, shown as `o`:

| `o` | Means |
|-----|-------|
| 3 | the default: the Linux finish with the chip kept awake, interrupt enables zeroed as ath6kl does |
| 7 | the same, interrupt enables left alone |
| 11 | preset 3 with nocash's sleep value written back (bit 0 clear): the v84-v88 finish |
| 0 | the old ath6kl-port sequence |

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

Step 2 has not been answered yet. The connect message bytes, header, mailbox
address and sequence match ath6kl's source line for line, and the mailbox reads
do pop frames. On v85 to v88 the byte-mode write went unanswered and a
block-mode write timed out. The code now follows ath6kl's poll cadence (one
poll every 10 ms, 2 s for an answer), zeroes the interrupt enables, pauses
100 ms between reading HTC_READY and answering, and tries five ways of writing
the frame in turn until one is answered. On v96, against a booted firmware,
the controller refused every CMD53 style and the CMD52 write went unanswered;
v97 (not yet run) replaced two "secure"-bit variants with the 4-bit styles:

| Style | Transfer |
|-------|----------|
| 0 | CMD53 byte mode, one padded block: what ath6kl ends up doing, since the mmc core only uses block mode for more than one block |
| 1 | CMD53 block mode, one block |
| 2 | CMD52, a byte at a time |
| 3 | the bus switched to 4-bit (CCCR 0x07, then the controller), then byte mode |
| 4 | 4-bit, block mode |

## The Wi-Fi Test screen

Settings, Wi-Fi Test. **A** (or a tap) runs the probe, **R** boots `Main.type4`,
**L** boots `Main.type1`, **X** cycles the boot preset, **B** leaves. A boot takes
several seconds; B also cuts the wait short.

| Line | Fields |
|------|--------|
| `core vN ph P clk C t/o T` | ARM11 core version, probe phase (4 = done), controller clock register, commands that timed out |
| `Cnn arg resp s0/s1` | the logged SDIO commands, green when answered; four rows once a boot has run |
| `tr S/N f C T @A:N x E/B` | boot step (3 HI, 4 STUB, 5 MAIN, 6 DONE) and sends completed, updated on every send; the first failed command (`0434` a CMD52, `0053` a CMD53 data write, `0405` a BMI reply that never came), its STAT0 \| STAT1 << 16 (bit `0x0040` of the top half is CMDTIMEOUT: the chip did not answer), the attempt and send count it failed at; reply words drained / the target-info byte count (12 expected) |
| `hb H sl S sc C` | the controller access in progress: step << 28 (1 waiting to send a command, 2 waiting for its response, 3 done; 4-8 the same for a data command, 6 moving data through the FIFO, 7 waiting for the transfer to end) \| register << 16 \| command word; `SYSTEM_SLEEP` and `LOCAL_SCRATCH` as read from the ROM |
| `drop n/id cc C t n/e` | frames drained before the connect and the last one's id; the card's CCCR after boot as bytes, FN1 block size low byte, bus width (0 = 1-bit, 2 = 4-bit), I/O ready, I/O enable (enumeration leaves `80000202`); write styles tried and a bit per style the controller refused |
| `s a b c d` | the four CMD53 styles (byte, block, 4-bit byte, 4-bit block): host int \| error << 8 \| lookahead valid << 16 \| MBOX_FRAME << 24 after the write, or, with bit 31 set, the controller's STAT1 STAT0 when it refused the write (`0040` in STAT1 is CMDTIMEOUT, `0002` CRC, `0008` data timeout; none of them with no DATAEND means the card held the line busy) |
| `ATHEROS 0271:0201 fn1 RDY wc W o O` | chip ID from the CIS, function 1 ready, `CFG11_WIFICNT` after the write, boot preset |
| `FW Tn STEP rdy R s N nc C [b 2] pl P` | image type, boot step reached, ready flag, mailbox sends, credit waits that ran out (each ends the attempt), `b 2` when the boot was redone from a cold start, credit polls made (climbing = the ARM11 is alive) |
| `HTC id I cr C sz S [ep E]` | HTC_READY: message id, credits, credit size, and the endpoint once connected |
| `r R mb M` | when no HTC_READY came: the interrupt state, and the stub readback (`0x21006136` when the stub landed) or a peek of mailbox 0 |
| `WMI 1001 MAC ...` or `HTC ep E st S sg G r R` | the MAC from WMI_READY; otherwise the connect endpoint, status (255 = nothing came back), how far the handshake got (1 READY to 5 WMI) and the interrupt state |

In probe mode the lower lines show the diagnostic-window reads (`diag`) and the
BMI target version and type (`BMI ver 0x2300006F ty 0x00000002`) instead.

On the NWM version tested here, the staged blobs sum to `0x9784AB50` with
`Main.type4` and `0x5C868B99` with `Main.type1`.

## Rules for changing the driver

Each of these was learned from a regression on hardware:

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

These blobs are Nintendo's copyright. They are never committed or embedded: users
dump NWM from their own console, run the extractor, and copy `STUBDATA.BIN`,
`STUBCODE.BIN`, `DATABASE.BIN`, `MAINTYP1.BIN` and `MAINTYP4.BIN` to
`SD:/Aurora/wifi/`. The Wi-Fi Test screen loads them from there into shared FCRAM
before each boot. Main.type5 is not used.

## Path forward

1. Resume by running v97 (Settings, Wi-Fi Test, **R**). `f`/`s` say why the
   controller refused the connect write, `cc` whether the card's bus width or
   function state changed at boot, and styles 3 and 4 whether 4-bit is what
   the running firmware needs. If 4-bit is answered, move the whole bring-up
   to 4-bit, as the Linux port runs.
2. Finish the HTC handshake: CONNECT_SERVICE answered, SETUP_COMPLETE, then
   `WMI_READY` with the MAC address.
3. WMI: channel parameters and a scan (AR6014 scans only the first channel set, so
   iterate), then connect to a BSS from the scan cache.
4. WPA2 4-way handshake (AES, SHA1, PRF), then DHCP, then ARP/IP/ICMP for a ping.
   A full TCP/IP stack for anything more.

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

The Wi-Fi driver code (the SDIO/BMI/HIF bring-up in `src/os/WiFi11.c`, plus the
`src/os/WiFi9.c`, the Wi-Fi parts of `src/os/os_main.c`, and `include/wifi.h`) is
licensed **GPL-2.0**, separately from the rest of AuroraOS. It derives register
facts and the BMI/HIF bring-up sequence from the **ath6kl legacy driver**
(GPL-2.0) as ported to the Nintendo 3DS by **Octoblimp**. Because GPL-2.0 is
copyleft, any binary that links this Wi-Fi code is covered by GPL-2.0.

Specific facts sourced from Octoblimp's ath6kl 3DS port: the BMI command-credit
register `COUNT_DEC + (HTC_MAILBOX_NUM_MAX + ENDPOINT1)*4 = 0x450`; mailbox 0 at
`0x800` (byte-increment, no end-address adjustment); `RX_LOOKAHEAD_VALID` at
`0x405`; the function-1 SDIO block size of 128; and the firmware target-address
map (`main_type1` -> `0x524C00`, `database` -> `0x53FE18`, host interest
`0x00500400`).

Facts sourced from the Linux 3DS port (GPL-2.0, techflashYT and Peugeot205GTI;
see *A working reference*): `CFG11_WIFICNT` (`0x10140180`) bit 0 as the Wi-Fi
subsystem power, `SDMMCCTL` bit 2 as the Wi-Fi SDIO power-off bit, `Main.type1`
as the image their container streams, the finish sequence that restores
`0x40c4` and `0x180c0` before `BMI_DONE`, the DATA32 block registers and the
SDIO-command bit of their SDHC driver, and ath6kl's HTC timing and interrupt
handling. That port in turn follows nocash's wifiboot, documented in GBATEK.

## Sources

Octoblimp's ath6kl legacy 3DS port (GPL-2.0); reference PDF and driver-plan notes
in `exclude/`; GBATEK "3DS Files: Module NWM", "3DS GPIO Registers", "3DS I2C MCU
Register Summary", "MBOX Transfer Headers"; 3dbrew NWM_Services, FIRM,
I2C_Registers; nesdev.org thread t=18490; Linux ath6kl driver (`htc_mbox.c`,
`hif.c`, `sdio.c`); the Linux 3DS port's AR6014 support (`Peugeot205GTI/linux-3DS`
PR 3, `Peugeot205GTI/arm9linuxfw` PR 2), which follows nocash's wifiboot.
