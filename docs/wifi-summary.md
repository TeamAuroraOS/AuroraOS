# Bare-metal 3DS Wi-Fi: current state

Status of the Wi-Fi effort in AuroraOS, a from-scratch, MIT-licensed 3DS operating
system.

## Working

The full SDIO hardware stack is functional and verified on real hardware. Starting
from nothing, AuroraOS now:

- Powers on the Wi-Fi chip. The key step is releasing its reset line at GPIO
  `0x10147028` bit 0. The chip stays in hardware reset until that bit is set.
- Enumerates it over SDIO (CMD5, CMD3, CMD7).
- Identifies it as an Atheros AR6014 (SDIO vendor `0x0271`, device `0x0201`), read
  from the card's CIS.
- Reads and writes its registers (CMD52, CMD53) and enables the Wi-Fi I/O function.

No prior from-scratch driver has been published that reaches this point; existing
homebrew routes Wi-Fi through Nintendo's sysmodule. The work is reverse-engineered
from the retail NWM module (using rizin) together with GBATEK and 3dbrew
documentation, cross-checked against the Linux ath6kl driver.

## Not done

Everything above the SDIO layer remains unimplemented. The chip is an inert
"thin-MAC" part: firmware must be uploaded into it (BMI), followed by a full protocol
stack (HTC, WMI, 802.11, WPA2, DHCP, TCP/IP). Progress currently stops at the first
BMI handshake. The SDIO transfers succeed, but the chip's bootloader does not respond,
because its exact control-register map differs from the open-source reference and must
be extracted from Nintendo's binary.

## Assessment

A working `ping` remains a large, multi-stage project consisting of a complete Wi-Fi
driver and TCP/IP stack. The hardware foundation, however, is verified and complete.
