/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Wi-Fi, ARM9 side: posts requests to the ARM11 (WiFi11.c) and reads results
 * back out of WifiShared.
 *
 * LICENSE: GPL-2.0, ath6kl-derived; credit Octoblimp. See docs/wifi.md
 * "License and credits".
 */
#include "aurora.h"
#include "audio.h"
#include "wifi.h"

extern void os_cache_sync(void);

static AudioCtrl *const ctrl = (AudioCtrl *)AUDIO_CTRL_ADDR;

/* Wi-Fi (GPL-2.0): wifi_probe/wifi_get are part of the GPL-2.0 Wi-Fi driver
 * (ath6kl-derived; credit Octoblimp). See docs/wifi.md "License and credits". */
void wifi_probe(void) {
  os_cache_sync();
  ctrl->cmd = AUDIO_CMD_WIFI;
  ctrl->cmd_seq = ctrl->cmd_seq + 1;
  os_cache_sync();
}

/* Trigger the firmware upload + boot. The caller (os_main) must have staged the
 * SD firmware into the WIFI_FW slots and set the WifiFw header first. */
void wifi_boot(void) {
  os_cache_sync();
  ctrl->cmd = AUDIO_CMD_WIFI_BOOT;
  ctrl->cmd_seq = ctrl->cmd_seq + 1;
  os_cache_sync();
}

void wifi_get(WifiShared *out) {
  os_cache_sync(); /* pull the shared block fresh from RAM */
  volatile unsigned char *s = (volatile unsigned char *)WIFI_SHARED_ADDR;
  unsigned char *d = (unsigned char *)out;
  for (unsigned i = 0; i < sizeof(WifiShared); i++)
    d[i] = s[i];
  out->sdmmcctl = *(volatile uint16_t *)0x10000020u; /* CFG9 SDMMCCTL (ARM9) */
}

