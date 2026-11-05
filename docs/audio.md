# Audio (CSND)

Sound is played by the CSND mixer at `0x10103000`, which lives on the ARM11. The
OS runs on the ARM9, so playback is driven from the ARM11 core through the
command block described in `include/audio.h`.

| Piece | File |
|-------|------|
| ARM9 API, core boot | `src/os/Audio9.c` |
| Output path + CSND | `src/os/Audio11.c` |
| WAV reader, shared by the OS and apps | `src/wavload.c` |
| The OS's own sounds | `src/os/UiSound.c`, `include/uisound.h` |
| Codec bus (shared with the touchscreen) | `src/os/Codec11.c` |

## The channel register layout

This is the part worth writing down, because the obvious reference is
misleading. libctru's `csnd.h` packs a channel setup as

```c
flags | SOUND_ENABLE | (CSND_TIMER(rate) << 16)
```

That word is the **argument to the CSND system module**, which decomposes it
before touching hardware. It is not the register layout. On bare metal, writing
it straight to the channel register does not work.

Channel `n` occupies `0x10103400 + n*0x20`:

| Offset | Width | Meaning | Notes |
|--------|-------|---------|-------|
| `+0x00` | 16 | Control flags | **bit 15 starts the channel** |
| `+0x02` | 16 | Period reload | **write-only**, and **two's complement** |
| `+0x04` | 32 | Volume | right in the low half, left in the high |
| `+0x0C` | 32 | Source address | physical |
| `+0x10` | 32 | Length in bytes | |
| `+0x14` | 32 | Loop address | physical |

Control-flag bits match libctru's `SOUND_*` values: bit 6 linear interpolation,
bits 10-11 loop mode (1 = loop, 2 = one-shot), bits 12-13 encoding (1 = PCM16),
bit 14 enable. Bit 15 has no libctru equivalent because the service sets it.

The period reload is the catch. The register counts **up** to overflow, as on
the NDS sound hardware this descends from, so it takes the negative of the
wanted period:

```c
uint32_t timer  = 0x3FEC3FC / rate;          /* libctru's CSND_TIMER */
uint32_t reload = (0x10000u - timer) & 0xFFFFu;
MMIO16(CSND_CH(n) + 0x02) = (uint16_t)reload;   /* before the start bit */
MMIO16(CSND_CH(n) + 0x00) = CH_START | CH_ENABLE | fmt | loopmode;
```

Write the reload **before** setting the start bit, or the channel begins on
whatever the previous sound left behind.

## Why a read-back check is not proof

`+0x02` is write-only and reads as zero. `+0x08` accepts a divider and reads it
back perfectly, and the channel ignores it completely.

So "the value I wrote is in the register" says nothing about whether the channel
uses that register, and chasing the read-back leads directly away from the
answer. The layout above was settled by **timing playback**, not by reading
registers back.

## Finding it again

The harness that worked this out lived in a Sound Test screen, removed once
audio was working. It is worth knowing how it went, because the same approach
applies to any register whose effect cannot be read back:

* Play a tone of an exactly known length and time it against the MCU real-time
  clock, counting with an ARM9 hardware timer over one RTC second, so the
  figure depends on no clock constant in this tree. (`src/os/Timer9.c` no
  longer calibrates against the RTC; the RTC loop it used is in the history of
  that file.) A ratio other than 1.0 is the factor by which the sample clock
  differs from what the divider assumes.
* Sweep every unclaimed offset in the channel block, writing the divider plain
  and two's-complement, timing each. The offset that yields the expected length
  is the register. That is what identified `+0x02` two's-complement.

A symptom worth recognising: playback at a period of exactly 65536 (about 7.8x
too slow for 8 kHz) means the reload register is reading zero, so the write is
not reaching the register the channel reads.

## The crash beep

A fault on either CPU plays three short beeps. The pattern is rendered once at
boot into `AUDIO_ERR_ADDR`, so playing it costs nothing but starting a channel.

That matters for the ARM11 path. CSND is a DMA engine, so a channel started
before a core stops still plays to completion; the ARM11 fault stub in
`audio11_start.s` therefore starts the tone with register stores alone, needing
no stack and no call, neither of which is safe in a faulted context. Those
stores hard-code the `AUDIO_ERR_*` values from `include/audio.h`, so the two
must change together.

The ARM9 path simply posts `AUDIO_CMD_ERROR` from `crash_handle()` before it
draws anything. When the ARM11 is what died that post is never serviced, which
is harmless because that core has already started the same sound itself.

Note that the ARM11 half only fires once the ARM11 exception vectors are
installed. `crash11_init()` in `Core11.c` is currently disabled, so an ARM11
fault is not caught at all today: no beep and no crash screen.

## Voices

Channel 0 belongs to the OS: the test tone, WAV playback in the File Explorer,
the Music app and the crash beep. Apps get **voices**, CSND channels 1 to 8,
through two more commands (`include/audio.h`):

| Command | Arguments |
|---------|-----------|
| `AUDIO_CMD_VOICE` | `arg0` the voice, loop and 16-bit flags, volume in the top half; `arg1` address; `arg2` bytes; `arg3` rate |
| `AUDIO_CMD_VOICE_STOP` | `arg0` a bit mask of voices |

A voice reads its samples where they sit, so they must be flushed from the
ARM9's cache and stay put while it plays. `AUDIO_VOICE_ANY` lets the core choose
among voices 1 to 7: the next one whose start bit reads clear, or simply the
next one. Whether that bit clears when a one-shot sound ends has not been
checked on hardware; if it does not, the choice is plain round-robin, which is
still correct. Voice 0 is never chosen, so a caller can keep it for music.

The command block holds a single request and the GPU uses it too, so a sender
waits for the core's acknowledgement before returning. Otherwise a present
posted straight after would overwrite the request before the core read it.

The Auric runtime keeps voice 0 for music and uses the rest for effects,
loading WAVs into the PCM buffer at `0x23400000`, which nothing else uses while
an app runs (`src/wavload.c` is the reader, shared with the File Explorer). The
Home Menu calls `audio_voices_stop()` at start-up, which is also where it lands
when an app returns, so an app's music cannot play on over memory the OS has
taken back. Voices arrived with core version 82; an older core acknowledges both
commands without acting on them, and apps stay silent. How an older core gets
replaced is under "Replacing a running core" below.

## The OS's own sounds

`src/os/UiSound.c` plays the seven sounds in `assets.pak` ([`assets.md`](assets.md),
"Sounds"). They use voices, which are free while the OS runs, so channel 0
(the Music app, Files) is never touched:

| Sound | Voice | Buffer | When |
|-------|-------|--------|------|
| startup | 2 | `0x27D80000`, 256 KB | once the Home Menu is drawn, when `audio_boot()` loaded or replaced the core; a HOME return finds the core running and stays quiet |
| Home music | 0, looped, at 70% | `0x27100000`, 5 MB, with its packed form at `0x27600000`, 1.5 MB | see "The Home Menu's song" below |
| install chime | 1 | `0x27DC0000`, 256 KB | when a download has passed its SHA-256 check and been renamed into place (`dl_finish()`) |
| select, back | 3 | `0x27E00000`, 256 KB for the three menu sounds | see "Menu sounds" below |
| move | 4 and 5 in turn | the same 256 KB | see "Menu sounds" below |
| aShop music | 0, looped | `0x27E40000`, 1.75 MB to `0x28000000` | from aShop's start (after the account check, before "Connecting to aShop...") until it closes |

`UI_SOUND_ADDR` (`0x27D80000`) to `0x28000000`, the end of Old 3DS FCRAM, is
used by nothing else. The buffers hold 4 s for each short sound, 4 s for the
three menu sounds together (they use 1.4 s) and 28 s for the music (the song is
17.5 s). A sound is decoded from the pack to 16-bit PCM before it plays.
`ui_sound_boot()`, called once the Home Menu is drawn, mounts the card itself,
starts the startup sound on a cold boot and then decodes the three menu sounds
one after another into their slot, 32-byte aligned. It runs at every OS start,
since an app may have used that memory. The music and chime are read when
aShop opens, on aShop's own mount, since mounting again would drop it. The
aShop song is 571,360 samples to decode; how long that takes on a console has
not been measured yet.

`audio_voice_play()` in `Audio9.c` waits for an in-flight present and then
for the core's ack, as the one-request block requires. `audio_voices_stop()`
and `audio_voices_stop_mask()` wait for a present too. A core running a Wi-Fi
command takes no request, so `UiSound.c` skips a sound while
`wifi_direct_active()`. When aShop closes it stops only voices 0 and 1, so the
back sound of leaving plays out. If a Wi-Fi command is still running then, the
music plays on until the next aShop visit or a boot stops it. Once started, a
channel needs nothing more from the core, so the music carries on through
joins and downloads. It also plays with the lid closed, like all sound but the
Home Menu's song (see "Brightness, the system menu and the lid").

All play at full volume, as the files were mixed, except the Home Menu's song.
The aShop song peaks at full scale, so the chime over a loud passage may clip;
that has not been heard yet. The menu sounds are quieter: select and back peak
at -14 dBFS, move at -25 dBFS.

Settings has four rows: Startup sound; Home music; aShop music, which covers
the chime too; and Menu sounds. They are bits 0, 3, 1 and 2 of `USER.dat` byte
52 (`USER_SOUND_*` in `include/user.h`). A set bit means off, so files from
before read as all on. A, a tap, Left or Right switches a row, and the change
is saved at once. The click comes after the switch, so turning Menu sounds on
is heard and turning it off is not. None of this needed a core change: voices
have been in the core since version 82.

### The Home Menu's song

`Aurora-HOME.wav` (39.3 s, by LeweckBanana, from an `.m4a` kept out of the
repository) plays on voice 0 whenever the Home Menu is on screen; it was heard
on the maintainer's console on 2026-11-01:

- After a cold boot it starts when the startup sound is over: `ui_sound_boot()`
  notes when that began and how long it lasts (2.6 s, of which the last 0.9 s
  is silence in the file). With the startup sound off, or on a HOME return, it
  starts as soon as it is loaded.
- It loops. The file is 17 ms of silence and then 18 bars at 110 BPM with no
  fade, so `TRIM_LEAD` cuts the silence and each pass stays on the beat. The
  jump across the seam is 143, less than the song's usual step between two
  samples (283), so the loop does not click.
- It stops when anything opens from the Home Menu: Settings, Files, Terminal,
  Music, 3D Model, aShop or an SD app (`ui_sound_home_leave()`, in `home_open`,
  `home_settings` and `home_terminal` in `os_main.c`). Folders and pop-ups are
  part of the Home Menu and leave it playing. Coming back restarts it from the
  top, with no startup sound.
- It stops when the lid closes (`get_keys()` calls `ui_sound_lid_closed()` before
  it waits) and restarts from the top on the next pass of the menu's loop after
  the lid opens. Other sound keeps playing with the lid closed.
- It plays at 70% (`HOME_VOLUME`, `0x5999`): the song peaks near full scale and
  the menu sounds play over it at full volume.
- Settings > Home music turns it off (`USER_SOUND_HOME_OFF`).

At 2.6 MB decoded it does not fit the sound slots, and FCRAM has no free stretch
that size on an Old 3DS, so it borrows 3D Model's file buffer (`GLB_FILE_ADDR`,
idle whenever the menu is showing). The first 5 MB of that buffer hold the PCM
and the next 1.5 MB the packed sound, room for 80 s; a compile-time check keeps
them inside it. Opening 3D Model overwrites them, so `ui_sound_home_lost()` has
the song loaded again on the way back. An app can overwrite them too, and the OS
starts afresh when one returns, so that case loads it again by itself.

Decoding 1.3 million samples in one go would hold up the menu at every start, so
it is split, all from `ui_sound_home_tick()`, which `home_tick` in `os_main.c`
calls on every pass of the menu's loop:

1. `home_fetch()` reads the packed sound in one go (0.73 MB). This is the only
   step that needs the card, and it is why the rest can be spread out: every
   other user of the card mounts and unmounts around its own work, so a file
   left open between passes would not survive. Blocks one pass.
2. `home_decode()` decodes for about a millisecond per pass
   (`asset_sound_decode()`, 32 frames at a time between looks at the clock), so
   the menu keeps answering.
3. Once it is all decoded, the startup sound is over and the core is free, it
   posts the voice. A core busy with a Wi-Fi command just makes it wait.

The whole decode is about 0.3 s of ARM9 time by estimate, so it should finish
well inside the startup sound; that has not been measured on a console.

### Menu sounds

In the Home Menu and its folders, Settings and every screen under it, and
aShop:

- **select** (`ui_sound_select()`): A, or a tap, that opens or does something:
  an app, a folder, a Settings row, a list row, a button, a menu item, the
  Yes of a confirm box, a drop with A while moving an app. Y (the action
  menu), START (Settings) and X (the Terminal) on the Home Menu, and Y and X
  in aShop, count too. A tap that both moves the selection and opens it makes
  only this sound.
- **back** (`ui_sound_back()`): B, or a tap on a Back button or outside a
  pop-up, that leaves a screen, folder or pop-up, or cancels: a folder up, the
  Home bar's back button (or holding an app over it), Settings and its
  screens, aShop and its screens, the No of a confirm box, a cancelled move.
- **move** (`ui_sound_move()`): the selection moving by the D-pad, circle pad,
  L or R, a swipe or a page dot, a value stepped with the D-pad (Clock,
  Brightness), and a page turned while an app is dragged to a side. Nothing
  plays when a key is pressed at an edge and nothing moves. Moves take turns
  on two voices, so with key repeat (every 70 ms) each 100 ms move sound plays
  out instead of being cut off.

A message closed with A, B or a tap (`ui_sound_close(keys)`) plays back for B
and select otherwise. `fv_confirm()` makes its Yes/No sound itself, so the
File Explorer's confirm boxes make it too. The Files screens, the Terminal, the
Music app, the 3D Model viewer, the keyboard, the setup wizard and the system
menu (Aurore Tools) make no menu sounds, apart from those confirm boxes.

## A core's start-up

A core sets its magic first thing, then initialises the codec (a soft reset
and about 90 ms of settle delays) and the touchscreen, and only then reaches
the loop that takes requests. So the magic only says the core is alive:
`audio_ready()` posts `AUDIO_CMD_NONE` and reports when it is taken, without
blocking, and `audio_wait_ready(ms)` waits for that. `audio_ready_ms()` is how
long it took from waking the core, shown on Settings > GPU Test. A core that
was already running counts as ready at once.

Up to core 123 the ARM11 ran without its caches, so every instruction came from
FCRAM and a nop loop ran about 80 times slower than one sized for a cached core.
Up to core 100 the settle delays were such loops (`spin(300000)` per
millisecond): GPU Test reported `Core answered after 8178 ms` on a cold boot,
eight seconds with no touch, no sound and, until the OS learned to wait, no GPU.
From core 101 `sleep_ms()` (src/os/Core11.c) counts on the core's MPCore private
timer (`0x17E00600`, half the CPU clock). It takes the New 3DS's 804 MHz so a
wait is never short; at 268 MHz it waits three times as long, about a third of a
second for the whole start-up. If the timer is found not to count, it falls back
to the old loop. The GPU's clock-up wait in `GPU_OP_INIT` uses it too. On the
console tested the core then answered within the boot's timer calibration (1.3
to 1.9 s, since removed); without it, a cold boot read
`Core answered after 265 ms`.

## The core's clock and caches

From core 124 the core no longer depends on how fast it happens to run. Two
things changed together, because each would have broken the other.

**A clock that holds at any speed.** The private timer runs free from start-up
with a prescaler of 134, and `core11_now_us()` folds its ticks into microseconds:
1 tick each at 268 MHz, 3 at 804 MHz, since the timer counts at half the CPU
clock. Everything the core times uses it. Before this, `wifi_now()` read the
timer directly, so at 804 MHz every Wi-Fi timeout ran out three times early, and
the main loop's rhythm (`spin(1200)` a pass) and the Wi-Fi driver's bounds were
counted in loop passes. They are all in time now: a main loop pass of 50 us, the
touchscreen and the system menu every 5 ms, the lid every 100 ms (it was really
about every 10 s at 268 MHz, not the 100-150 ms its comment claimed), and
`WAIT_CMD_*` for the SDIO controller. `wifi_ms()` waits seven times what it says,
because the boot and the join were proven with a counted loop that ran 6-8 times
long (`docs/wifi.md`). The stereo mask's period scales with the clock too, so it
stays at the 10 ms it was proven with. `sleep_ms()` keeps its old length at both
clocks, on purpose.

**Caches.** `src/os/Mmu11.c` turns on the MMU, the L1 caches and branch
prediction, with a flat map in which only the core's image, the 2 MB at
`0x23000000`, is cached. Everything else stays strongly ordered, which is how
every access behaves with the MMU off, so what the core shares with the ARM9 and
the GPU works as before and needs no cache maintenance: `dcache_clean()` and its
kin only order writes now. `AudioCtrl.core_flags` says what is on
(`AUDIO_FLAG_CACHES`, `AUDIO_FLAG_L2`), and `systemctl status core11` shows it.

Holding SELECT at boot leaves the core's caches and New 3DS full power off, as it
does the ARM9's MPU, so a build that misbehaves with them still starts.

Some things run with the map off: `AUDIO_CMD_N3DS` (libn3ds warns against
switching with a map on), the other cores' start, `AUDIO_CMD_RUN11`, and parking.
`mmu11_off()` does that through `mmu11_stop` in `audio11_start.s`, which cleans
L1, then any L2, then turns everything off without touching data memory in
between, since a dirty line made after the clean would be lost.

**The L2 cache.** On a retail New 3DS at 804 MHz `mmu11_on()` also starts the
2 MB L2C-310 at `0x17E10000`, after invalidating every way. Horizon's kernel is
documented as only setting its enable bit, and Aurora does not change the
controller's auxiliary or latency settings. An `AUDIO_CMD_RUN11` job still runs
with the L2 off, as the CPU stress test's results say.

## The other ARM11 cores

From core 124 `AUDIO_CMD_SMP` (15; `include/smp.h`, `src/os/Smp11.c`) starts
cores 1 to 3 and gives them jobs, while core 0 stays what it always was. An Old
3DS has core 1, a New 3DS all three. They only run when an app asks.

They wait in `smp11_stub`, a small loop copied out of the core's image to
`0x233E1000`, so a core 0 replaced by another build leaves them alone. It uses
WFI, because a clock mode change only applies with every core in WFI, and is
woken by a software interrupt.

| `arg0` | `arg1` | Does |
|--------|--------|------|
| `SMP_OP_CORES` | mask of cores | starts them; `SmpBlock.started` and `failed` say what happened |
| `SMP_OP_RUN` | an `SmpJob` | runs `entry(arg, core)` on each named core, with its stack and FCRAM map; the ack comes as they start |
| `SMP_OP_END` | | sets `stop`, waits up to 1 s for the jobs to return, and leaves nothing of theirs in the L2 |
| `SMP_OP_TEST` | | starts every core and measures it; `systemctl status cores` shows the result |

A job gets the MMU, the caches, SMP coherency, branch prediction, the VFP in
RunFast mode, and unaligned access (SCTLR.U set), with interrupts off. Its map
marks FCRAM sections `SMP_MAP_CACHED` (write-back, coherent between the cores) or
`SMP_MAP_SHARED` (normal, not cached: what the ARM9, CSND or the GPU also touch);
the rest is strongly ordered. An exception in a job hangs that core.

Starting them follows libn3ds (`source/arm11/drivers/pdn.c`). Core 1 waits in the
boot ROM for an entry at `0x1FFFFFDC` and a software interrupt, the path
open_agb_firm uses on every model. Cores 2 and 3 are held in reset until the boot
ROM overlay points them at an entry, at a 268 MHz clock mode. libn3ds keeps that
path compiled out, so here every step of it is bounded and a core that does not
answer is only reported. FCRAM keeps `alive` across a reset, so a core only counts
as started if it answers a software interrupt now.

## Replacing a running core

The ARM11 can still be running a core from before a reboot, and FCRAM keeps the
command block's magic even when it is not. `audio_boot()` therefore does not
trust the magic alone:

1. With the magic present it posts `AUDIO_CMD_NONE` and waits for the ack. No
   ack means no core is running (the ARM11 was reset and is waiting in the firm's
   stub), so the core is loaded as on a cold boot.
2. A core that answers with this build's `AUDIO_CORE_VERSION` is left alone.
   That is the normal case after a HOME return.
3. A core with a different version, from `AUDIO_PARK_VERSION` (83) on, is sent
   `AUDIO_CMD_PARK`. It stops every channel, copies a small position-independent
   loop to `AUDIO_PARK_ADDR` (`0x233E0000`), clears the magic, acks and jumps
   there. The loop sets `AUDIO_ST_PARKED` and waits on the firm's mailbox exactly
   as `src/arm11_start.s` does at boot. Once the ARM9 sees that status the old
   image is free, and the new core is copied over it and woken the usual way.
4. A core older than 83 does not know the park command, so it cannot be moved
   out of the way while it runs. `audio_boot()` returns `AUDIO_BOOT_STALE`, and
   the Home Menu offers to power off, which is the only way to load the new one.

The park loop is copied out before the ack, so after the ARM9 sees the ack the
old core has only a few instructions left to run in its own image, and the ARM9
still waits for the loop to report from its new address before writing there.
The version check and the power-off offer run at every boot. The park hand-off
itself only happens when the core version changes; it has worked on hardware
since core 84 replaced 83 without a power-off.

| Command | Arguments |
|---------|-----------|
| `AUDIO_CMD_NONE` | none; acked, which shows a core is running |
| `AUDIO_CMD_PARK` | none; the core stops and waits for a new one |

## Running an app's code on the ARM11

From core 116, `AUDIO_CMD_RUN11` (13) calls ARM11 code that an app left in
FCRAM: `arg0` is its address (ARM code, word aligned, in the first 128 MB of
FCRAM), `arg1` the one argument it gets. `src/os/Run11.c` gives the call what
Horizon gives an app, and takes it away again before the ack:

- **The MMU, with a flat map**: every 1 MB section at its own address,
  strongly ordered, except FCRAM's first 128 MB (on every model), which is
  normal write-back, write-allocate memory, not shared. The table (16 KB, in
  the core's `.bss`) is built the first time. Domain 0 is a manager domain,
  so nothing is permission-checked.
- **The L1 caches and branch prediction** (SCTLR `M`, `C`, `I`, `Z`, and `XP`
  for the ARMv6 descriptor format). The New 3DS's L2 cache is off for the job,
  and the core's own map (Mmu11.c, from core 124) makes way for it.
- **The VFP**: CPACR gives cp10 and cp11 to every mode and FPEXC.EN is set.
  It stays enabled afterwards; the core's own code never uses it. The code
  run should put the VFP in RunFast mode (flush-to-zero, default NaN), since
  there is no support code for the cases that would otherwise trap.

Before the call every cache and the TLB are cleaned or invalidated; after it
the D-cache is cleaned and invalidated so the results are in memory, the
SCTLR, TTBR0, TTBCR and DACR go back to what they were, and the caches and
TLB are invalidated again. If the MMU was already on (someone else's map),
the code just runs as things are. Interrupts stay masked throughout, and
while the code runs the core does nothing else: no presents, no touch, no
new sound. An exception in the code hangs the core (its vectors are not
installed). The CPU stress test (`cpu-stress/`) is the user: it runs its
workloads this way to measure the ARM11 as Horizon apps get it, and did so
on a New 3DS at 804 MHz (2026-10-08).

`AUDIO_CMD_N3DS` takes mode 0 from core 116 too, to go back to 268 MHz; it
leaves the L2 controller's power and the GPU's New 3DS bits as they are.

## Brightness, the system menu and the lid

Three later jobs have nothing to do with sound, but they are ARM11 work, so they
live in the core too:

- **Core 117**, `AUDIO_CMD_BRIGHTNESS` (14): `arg0` is a level from 0 to 4.
  From core 119 the core turns ABL off, makes sure the PWM is enabled, writes
  a calibrated duty per panel (another one for the top in 3D), and copies the
  registers back into `diag4` to `diag7`. See [`power.md`](power.md),
  "Screen brightness".
- **Core 118**, the system menu inside apps (`src/os/SystemMenu11.c`). Every 16
  passes of the command loop, before it looks for a command, the core reads
  the buttons. While an app started from the Home Menu runs (`app_active` in
  the block at `0x233A8000`, `include/system_menu.h`), L + Down + Select opens
  the menu on the bottom screen. The core takes no command while the menu is
  open, and it can power off by itself, because `src/i2c.c` and `src/power.c`
  are linked into it. From core 119 it offers Return to Home only to an app
  whose runtime set `app_caps`, and it closes the menu as soon as
  `app_active` drops (an older app left by HOME meanwhile), so the restarting
  OS's probe is answered. See [`home.md`](home.md), "The system menu".
- **Core 123**: only the Wi-Fi code changed ([`wifi.md`](wifi.md), "When a
  session stops working").
- **Core 122**: the crash tone (`audio11_error_play()`) stops every channel
  first, so a looping song does not play on under the crash screen.
- **Core 120**, the lid (`core11_sleep_poll()` in `src/os/Core11.c`, PR 29).
  Every 32,768 loop passes, and on each of the system menu's button reads,
  the core reads the lid sensor (GPIO1 bit 2). Closed, it sets both
  backlights' duty to 0; open again, it restores the level in use. It keeps
  taking commands meanwhile, so sound goes on playing; the ARM9 side waits at
  its button reads. See [`power.md`](power.md), "Closing the lid".
- **Core 121**, a TCP connection for HTTPS (`WIFI_NETOP_TCP`,
  `AUDIO_TCP_VERSION`): the command keeps one connection open, moving bytes
  through two rings at `0x233F0000`, until the ARM9 closes it, while the ARM9
  runs TLS over it. No audio change. See [`wifi.md`](wifi.md), "A connection
  the ARM9 drives", and [`tls.md`](tls.md).

The core links those two shared files, `include/font.h` and
`include/system_menu.h`, so a change to any of them is a core change and needs
an `AUDIO_CORE_VERSION` bump.

## Audio files

`.aaf` is a 16-byte header (`AAF1`, version, channels, bit depth, reserved,
sample rate, sample count) followed by raw mono PCM. `audio/aaf_tool.py`
converts and plays them on a PC; `load_track()` in `src/os/os_main.c` reads them
from `SD:\Aurora\Music`.
