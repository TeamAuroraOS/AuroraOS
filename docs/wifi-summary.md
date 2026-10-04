# Bare-metal 3DS Wi-Fi: current state

Status of the Wi-Fi effort in AuroraOS, a from-scratch, GPL-3.0 3DS operating
system whose Wi-Fi driver is GPL-2.0. The details are in [wifi.md](wifi.md).

## Working

Verified on real hardware (New 3DS). Starting from nothing, AuroraOS:

- Powers the Wi-Fi subsystem (`CFG11_WIFICNT` bit 0) and pulses the chip's reset
  line (GPIO `0x10147028` bit 0).
- Enumerates the chip over SDIO (CMD5, CMD3, CMD7) and identifies it as an
  Atheros AR6014 (vendor `0x0271`, device `0x0201`) from its CIS.
- Reads and writes its registers (CMD52, CMD53) and its memory, through the
  diagnostic window.
- Copies that firmware from the console's own NWM system module the first time
  Settings > Wi-Fi opens: it reads the system NAND (never writes it),
  decrypts NWM and unpacks its code, and saves the blobs to the SD card
  (checked on the PC; not yet run on a console).
- Talks to its bootloader (BMI), uploads the NWM firmware from the SD card,
  registers the firmware's patch (the "database" blob, a DataSet list the
  firmware applies once `hi_dset_list_head` points at it) and starts it.
- Completes the HTC handshake and receives `WMI_READY` (core v105, 2026-10-03):
  the firmware is running and reports the console's MAC address, which matches
  the console's own settings and the module's EEPROM.
- Scans the 2.4 GHz channels and lists the networks nearby with channel,
  signal, BSSID, security and name (core v108).
- Joins an open network, gets an address over DHCP, finds the router with
  ARP and pings it (core v110, a phone hotspot, 2026-10-03), with its own
  small IP stack on the ARM11.

No earlier from-scratch driver has been published that reaches this point;
existing homebrew routes Wi-Fi through Nintendo's sysmodule. The work combines
reverse engineering of the retail NWM module with GBATEK, 3dbrew, the ath6kl
driver, and the Linux 3DS port's AR6014 support.

## Where it stands (2026-10-03)

- Two bugs stood between HTC_READY and WMI. An ARM11 compiler setting let GCC
  emit unaligned stores that this core's alignment mode silently misplaces,
  corrupting BMI command headers (fixed with `-mno-unaligned-access`). And the
  boot sequence copied from the Linux port skipped the firmware patch, so the
  firmware failed right after HTC_READY.
- Core v111 stays joined after the join, looks up names over DNS and pings
  any address; the terminal has a Linux-style `ping` (works on hardware).
- Core v112 joins WPA2-PSK networks: the password is typed on the console,
  the ARM9 makes the PMK, and the ARM11 runs the 4-way handshake and installs
  the keys in the firmware; it joined a WPA2/WPA3 home router on hardware.
- **Settings > Wi-Fi** is the console-side setup: search, pick a network,
  type a secured network's password on the keyboard, join.
- The debug log (`WiFi_Log.txt`) that got it here was removed in core v113
  once everything worked.

## Not done

WPA3, servicing the chip between commands, and TCP. The hardware foundation, the firmware boot, the WMI channel, the
scan, the join and the data path are in place.
