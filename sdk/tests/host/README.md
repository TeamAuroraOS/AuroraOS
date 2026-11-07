# Host tests for the C SDK runtime

The SDK runtime's real code, and the OS code it links, built for the PC and run
against stand-ins for the hardware. Nothing here runs on a console, and nothing
here replaces trying the code on one: these tests catch logic mistakes early,
not hardware ones.

```
py sdk/tests/host/run.py            # every test
py sdk/tests/host/run.py sockets    # one; -v shows the compiler commands
```

It needs devkitPro's msys gcc (`C:\devkitPro\msys2\usr\bin\gcc.exe`, or set
`MSYS_BIN`) and devkitARM's headers (found next to `arm-none-eabi-gcc`, or set
`DEVKITARM`). The build goes to `sdk/build/host`, which is git-ignored; `--keep`
reuses it, so the copied sources there can be changed by hand to check that a
test fails when the code is wrong.

## How it works

`run.py` copies the files under test (the `SOURCES` list) into the build folder
with their inline assembly taken out (cache and coprocessor operations), and
builds them with the same include order as `sdk/aurcc.py`. The runtime's POSIX
names (`socket`, `poll`, `opendir`, ...) would clash with the PC's C library, so
`rename.h` gives them other names in the code under test and in the tests alike.
FatFs is built from a copy of `ffconf.h` with `f_mkfs` turned on, and `<dirent.h>`
is devkitARM's own `sys/dirent.h`, with libsysbase's `DIR_ITER` lifted out of
`iosupport.h`, so `app_dirent.c` fills in the `DIR` it fills in on the console.

Stand-ins:

| File | For |
|------|-----|
| `core_model.c`, `model.h` | the ARM11 core's network side: `AUDIO_CMD_WIFI_NET` with `STATUS`, `DNS`, `PING`, `TCP_BG`, `UDP_BIND`, `UDP_UNBIND` and `LEAVE`; the background TCP connection through the real `WifiTcp` rings and the UDP port through `WifiUdp`, the way `WiFi11.c` behaves (the `alive` watchdog, FIN and RST, what each core version lets run beside a connection); scripted servers and UDP peers behind it. It maps `0x23000000`-`0x24000000` at its real address. |
| `stubs.c` | the OS: the timer, `os_cache_sync`, `delay`, TLS (not tested here), the join |
| `stubs_rt.c`, `hostrt.h` | the runtime files not built here (`app_main.c`, `app_sys.c`, ...): an app started from the Home Menu, no HOME press, app time on the simulated clock, a lid a test can close, `host_frames()` for an idle frame loop |
| `ramdisk.c` | the SD card: a RAM disk under the real FatFs, formatted FAT32 |
| `check.h` | `CHECK`, `CHECK_EQ`, and scenarios, each run in a process of its own (`fork`), so what one leaves behind cannot change the next |

Time is simulated: it moves on whenever the code under test reads the clock,
waits or syncs its cache, and the model core takes a step each time, as the
real one runs beside the ARM9. A test can also move it on by itself
(`host_advance`). So the tests check order and logic, not speed.

The ARM11 core's own code can be tested where it is plain C: `test_net11.c`
links `src/os/Net11.c` alone (`ONLY` in `run.py`, since the core and the app
side share function names), and builds and parses UDP frames with it.

## The tests

| File | What |
|------|------|
| `test_sockets.c` | `app_socket.c` over the real `AppNet.c`, `WiFiTcp.c`, `WiFiUdp.c` and `app_net.c`: addresses and names, TCP and UDP, blocking and not, timeouts, `poll`/`select`, the one-connection rule, keep-alive, ways of closing, cores 123, 125 and 126 |
| `test_dirent.c` | `app_dirent.c` over the real `app_fs.c` and FatFs |
| `test_net11.c` | the core's UDP frames: `net_udp` and `net_rx` |

A new test is a `test_NAME.c` with a `SCENARIOS("NAME", S(f), ...)` line at the
end; `run.py` finds it. Code it needs that is not built yet goes into `SOURCES`.
