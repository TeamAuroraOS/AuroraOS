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
- Talks to its bootloader (BMI), uploads the NWM firmware from the SD card and
  starts it (all 378 sends on the first attempt since core v96).
- Receives the running firmware's first message, `HTC_READY` (core v84): 10
  credits of 1544 bytes.

No earlier from-scratch driver has been published that reaches this point;
existing homebrew routes Wi-Fi through Nintendo's sysmodule. The work combines
reverse engineering of the retail NWM module with GBATEK, 3dbrew, the ath6kl
driver, and the Linux 3DS port's AR6014 support.

## Paused (2026-09-30)

- The rest of the HTC handshake (connect the WMI control service, setup
  complete, `WMI_READY` with the MAC address) is written. After boot the SD
  controller refuses the CMD53 write of the connect request, and a CMD52 write
  goes unanswered. Core v97, built but not yet run on hardware, records why
  the controller refuses it and tries a 4-bit bus, as the Linux port uses.

## Not done

WMI, 802.11 scan and association, WPA2, DHCP, and IP, which a `ping` needs. That
remains a large, multi-stage project: a complete Wi-Fi driver and a TCP/IP stack.
The hardware foundation and the firmware boot are in place.
