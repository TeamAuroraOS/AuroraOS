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
  starts it.
- Receives the running firmware's first message, `HTC_READY` (core v84): 10
  credits of 1544 bytes.

No earlier from-scratch driver has been published that reaches this point;
existing homebrew routes Wi-Fi through Nintendo's sysmodule. The work combines
reverse engineering of the retail NWM module with GBATEK, 3dbrew, the ath6kl
driver, and the Linux 3DS port's AR6014 support.

## In progress

- The firmware upload stopped completing after core v88. Two regressions have
  been found and fixed; core v95 records exactly where the upload stalls and
  whether the chip or the controller gave up.
- The rest of the HTC handshake (connect the WMI control service, setup
  complete, `WMI_READY` with the MAC address) is written. The target has not
  answered the connect request yet.

## Not done

WMI, 802.11 scan and association, WPA2, DHCP, and IP, which a `ping` needs. That
remains a large, multi-stage project: a complete Wi-Fi driver and a TCP/IP stack.
The hardware foundation and the firmware boot are in place.
