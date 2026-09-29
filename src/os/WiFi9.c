/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: GPL-2.0, ath6kl-derived; credit Octoblimp. See docs/wifi.md
 * "License and credits". */
#include "aurora.h"
#include "audio.h"
#include "wifi.h"

extern void os_cache_sync(void);

static AudioCtrl *const ctrl = (AudioCtrl *)AUDIO_CTRL_ADDR;

void wifi_probe(void) {
  os_cache_sync();
  ctrl->cmd = AUDIO_CMD_WIFI;
  ctrl->cmd_seq = ctrl->cmd_seq + 1;
  os_cache_sync();
}

/* The firmware must already be staged. opts goes in the command block, not
 * WifiShared, where a line the ARM11 has dirtied could be written back over
 * it. */
void wifi_boot(u32 opts) {
  os_cache_sync();
  ctrl->arg0 = opts;
  ctrl->cmd = AUDIO_CMD_WIFI_BOOT;
  ctrl->cmd_seq = ctrl->cmd_seq + 1;
  os_cache_sync();
}

void wifi_get(WifiShared *out) {
  os_cache_sync();
  volatile unsigned char *s = (volatile unsigned char *)WIFI_SHARED_ADDR;
  unsigned char *d = (unsigned char *)out;
  for (unsigned i = 0; i < sizeof(WifiShared); i++)
    d[i] = s[i];
  out->sdmmcctl = *(volatile uint16_t *)0x10000020u; /* CFG9 SDMMCCTL (ARM9) */
}

