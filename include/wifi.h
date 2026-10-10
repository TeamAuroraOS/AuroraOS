/* SPDX-License-Identifier: GPL-2.0 */
/* LICENSE: The Wi-Fi code is GPL-2.0, separately from the rest of AuroraOS. It
 * derives register facts and the BMI/HIF bring-up sequence from the ath6kl
 * legacy driver as ported to the 3DS by Octoblimp, and more from the Linux 3DS
 * port (techflashYT, Peugeot205GTI). Credit: Octoblimp; ath6kl (GPL-2.0); the
 * Linux 3DS port. See docs/wifi.md "License and credits". */
#ifndef AURORA_WIFI_H
#define AURORA_WIFI_H

#include <stdint.h>

#define WIFI_SHARED_ADDR 0x233B0000u
#define WIFI_SDIO_BASE   0x10122000u /* logical; = physical 0x1EC22000 */

/* Firmware staging, between the audio PCM buffer and the app-launch stage.
 * The blobs are Nintendo copyright: loaded from SD at runtime, never
 * embedded. */
#define WIFI_FW_ADDR      0x23E00000u
#define WIFI_FW_MAGIC     0x46574631u /* "1FWF": ARM9 staged all four blobs */
#define WIFI_FW_STUBDATA (WIFI_FW_ADDR + 0x00001000u)
#define WIFI_FW_STUBCODE (WIFI_FW_ADDR + 0x00002000u)
#define WIFI_FW_DATABASE (WIFI_FW_ADDR + 0x00003000u)
#define WIFI_FW_MAIN     (WIFI_FW_ADDR + 0x00010000u)

enum {
  WIFI_FW_TYPE1 = 1,
  WIFI_FW_TYPE4 = 4,
};

typedef struct {
  volatile uint32_t magic;
  volatile uint32_t stubdata_len;
  volatile uint32_t stubcode_len;
  volatile uint32_t database_len;
  volatile uint32_t main_len;
  volatile uint32_t main_type;
} WifiFw;

/* Phase the ARM11 probe reached, so a hang localises to the last phase set. */
enum {
  WIFI_PH_NONE = 0,
  WIFI_PH_REGS = 1,
  WIFI_PH_CLK  = 2,
  WIFI_PH_CMD  = 3,
  WIFI_PH_DONE = 4,
};

#define WIFI_LOG_MAX 12
#define WIFI_BSS_MAX 16

/* One network from a BSSINFO event: the firmware's header, then the SSID and
 * capability from the beacon or probe response it carries. */
typedef struct {
  volatile uint8_t bssid[6];
  volatile uint16_t channel;   /* MHz */
  volatile int16_t rssi;       /* dBm */
  volatile uint8_t snr;
  volatile uint8_t ssid_len;
  volatile uint16_t caps;      /* 802.11 capability: bit 4 = privacy */
  volatile uint8_t frame_type; /* 1 beacon, 2 probe response */
  volatile uint8_t seen;       /* events for this BSSID */
  volatile uint8_t ssid[32];
  volatile uint8_t sec;        /* WIFI_SEC_* */
  volatile uint8_t pad;
} WifiBss;

/* A network's security, from its capability field and RSN or WPA element. */
enum {
  WIFI_SEC_OPEN = 0,
  WIFI_SEC_WEP,      /* privacy, but neither element                  */
  WIFI_SEC_WPA1,     /* only the old WPA element                      */
  WIFI_SEC_WPA2,     /* RSN with PSK and CCMP: what the driver joins  */
  WIFI_SEC_WPA3,     /* RSN with SAE and no PSK                       */
  WIFI_SEC_EAP,      /* RSN with 802.1X (enterprise) and no PSK       */
  WIFI_SEC_TKIP,     /* RSN with PSK, but no CCMP pairwise cipher     */
};
typedef struct {
  volatile uint16_t cmd;   /* command word low 16 bits (0 = empty slot) */
  volatile uint16_t ok;    /* 0 empty, 1 CMDRESPEND, 2 error/timeout */
  volatile uint32_t arg;
  volatile uint32_t resp;  /* RESP0 | RESP1<<16 */
  volatile uint16_t stat0;
  volatile uint16_t stat1;
} WifiCmd;

typedef struct {
  volatile uint32_t phase;
  volatile uint32_t seq;
  volatile uint32_t timeouts;
  volatile uint32_t clk;
  volatile uint32_t nlog;
  WifiCmd log[WIFI_LOG_MAX];
  volatile uint32_t sdmmcctl; /* CFG9 SDMMCCTL 0x10000020 (ARM9-filled) */
  volatile uint16_t gpio_before;
  volatile uint16_t gpio_after; /* GPIO_DATA4 0x10147028: bit0 = Wi-Fi reset */
  volatile uint32_t cis_addr;
  volatile uint8_t cis[48];
  volatile uint16_t manf;       /* CISTPL_MANFID: 0x0271 = Atheros */
  volatile uint16_t card;       /* CISTPL_MANFID: 0x0201 = AR6014 */
  volatile uint8_t ior;         /* CCCR 0x03 I/O Ready: bit1 = fn1 ready */
  volatile uint8_t hif[16];     /* function-1 HIF regs 0x400..0x40F */
  volatile int32_t bmi_wr;      /* BMI CMD53 write result (0 ok, neg err) */
  volatile int32_t bmi_rd;
  volatile uint32_t bmi_ver;
  volatile uint32_t bmi_type;
  volatile uint32_t bmi_look;   /* first word of the target-info reply */
  volatile uint16_t bmi_s0;
  volatile uint16_t bmi_s1;
  volatile uint16_t bmi_ctl;
  volatile uint16_t bmi_idx;
  volatile uint32_t bmi_credit; /* first credit read from 0x450 */
  volatile uint8_t cnt[8];      /* COUNT regs 0x420..0x43F low bytes */
  volatile uint32_t diag_a;     /* diag-window read of target 0x00500400 */
  volatile uint32_t diag_b;     /* diag-window read of target 0x00520100 (BMI-write test) */
  volatile uint32_t boot_step;  /* WIFI_BOOT_* */
  volatile uint32_t boot_exec;  /* BMIExecute return param from the NWM stub */
  volatile uint32_t boot_ready; /* HI+0x58 poll: 1 = NWM firmware signalled ready */
  volatile uint32_t htc_ready;  /* 1 = an HTC control message was found on mailbox 0 */
  volatile uint32_t htc_look;   /* RX lookahead 0x408 (HTC frame header of a pending msg) */
  volatile uint32_t htc_msgid;  /* HTC message ID (1 = HTC_MSG_READY) */
  volatile uint32_t htc_credits;/* HTC_READY CreditCount */
  volatile uint32_t htc_credsz; /* HTC_READY CreditSize  */
  volatile uint32_t htc_regs;   /* 0x400 | 0x401<<8 | 0x402<<16 | 0x405<<24 */
  volatile uint32_t bmi_sends;
  volatile uint32_t bmi_nocred; /* sends that ran out the credit wait with no credit */
  volatile uint32_t fw_chk;     /* unused */
  volatile uint32_t fw_dbrd;    /* stub_code word 0 read back via diag */
  volatile uint32_t fw_dbex;    /* stub_code word 0 as staged */
  volatile uint32_t fw_type;    /* selected NWM Main.type image */
  volatile uint32_t boot_opts;  /* WIFI_OPT_* the last boot ran with */
  volatile uint32_t wificnt;    /* CFG11_WIFICNT before<<8 | after, bit0 = on */
  volatile uint32_t htc_ep;     /* endpoint the target gave WMI_CONTROL_SVC */
  volatile uint32_t htc_status; /* HTC connect-service status, 0 = connected */
  volatile uint32_t htc_maxmsg; /* largest message that endpoint accepts */
  volatile uint32_t htc_stage;  /* WIFI_HTC_* */
  volatile uint32_t wmi_event;  /* first WMI event id (0x1001 = WMI_READY) */
  volatile uint32_t wmi_mac0;   /* MAC bytes 0-3 */
  volatile uint32_t wmi_mac1;   /* MAC bytes 4-5 */
  volatile uint32_t htc_regs2;  /* CCCR after boot: IOE | IOR<<8 | width<<16 | blksz<<24 */
  volatile uint32_t htc_look2;  /* lookahead (0x408) after HTC_READY was read */
  volatile uint32_t htc_drained;/* frames dropped before connect | last id << 16 */
  volatile uint32_t htc_try;    /* how many write styles the connect tried */
  volatile uint32_t htc_err;    /* bit per style whose transfer the controller refused */
  /* Per write style: host int | err<<8 | lookahead<<16 | frame<<24, or, when
   * the controller refused the write, bit 31 | STAT0 | STAT1<<16. */
  volatile uint32_t htc_snap[5];
  volatile uint32_t boot_tries;  /* 1, or 2 when the upload had to be redone */
  volatile uint32_t trace;       /* boot step << 24 | sends, published every send */
  /* The first command that failed: the controller command word, 0x0053 or
   * 0x1053 for a CMD53 write or read, 0x0405 for a BMI reply that never came. */
  volatile uint32_t fail_cmd;
  volatile uint32_t fail_arg;    /* its argument; for a CMD53 the address | -err << 28 */
  volatile uint32_t fail_stat;   /* STAT0 | STAT1 << 16 at that moment */
  volatile uint32_t fail_at;     /* attempt << 28 | boot step << 24 | sends done */
  volatile uint32_t bmi_polls;   /* credit polls this attempt, published live */
  volatile uint32_t bmi_extra;   /* reply words left in the mailbox and drained */
  volatile uint32_t bmi_bc;      /* target-info byte count (ath6kl expects 12) */
  volatile uint32_t soc_sleep;   /* SYSTEM_SLEEP (0x40c4) as the ROM had it */
  volatile uint32_t soc_scratch; /* LOCAL_SCRATCH (0x180c0) as the ROM had it */
  volatile uint32_t hb;          /* WIFI_HB_* << 28 | register << 16 | command */
  /* The scan after WMI_READY. */
  volatile uint32_t wmi_stage;   /* WIFI_WMI_* */
  volatile uint32_t wmi_events;  /* WMI events received after WMI_READY */
  volatile uint32_t wmi_last;    /* id of the last of them */
  volatile uint32_t wmi_cmderr;  /* last WMI_CMDERROR: command | error << 16 */
  volatile uint32_t wmi_errors;  /* how many WMI_CMDERROR events came */
  volatile uint32_t scan_status; /* SCAN_COMPLETE status, 0xFFFFFFFF if none came */
  volatile uint32_t htc_credit;  /* credits the host has for the WMI endpoint */
  volatile uint32_t bss_seen;    /* BSSINFO events, duplicates included */
  volatile uint32_t bss_count;   /* entries in bss[] */
  WifiBss bss[WIFI_BSS_MAX];
  /* The data services (best effort, background, video, voice): the endpoint
   * each was given, and the connect status (0xFF: no answer). */
  volatile uint8_t svc_ep[4];
  volatile uint8_t svc_status[4];
  /* Counter 0 (COUNTER_INT_STATUS bit 0) is the firmware's assert signal. */
  volatile uint32_t fw_assert;   /* 1 once the signal was seen */
  volatile uint32_t fw_assert_t; /* when, in microseconds into the command */
  /* Joining the requested network (WIFI_OPT_CONNECT). Addresses are
   * a << 24 | b << 16 | c << 8 | d for a.b.c.d. */
  volatile uint32_t conn_stage;   /* WIFI_CONN_* */
  volatile uint32_t conn_tries;   /* WMI_CONNECT commands sent */
  volatile uint32_t conn_reason;  /* last DISCONNECT: reason | 802.11 status << 16 */
  volatile uint32_t conn_channel; /* MHz, from the CONNECT event */
  volatile uint8_t conn_bssid[6];
  volatile uint8_t gw_mac[6];
  volatile uint32_t ip;
  volatile uint32_t mask;
  volatile uint32_t gw;
  volatile uint32_t dns;
  volatile uint32_t dhcp_server;
  volatile uint32_t lease;        /* seconds */
  volatile uint32_t ping_sent;
  volatile uint32_t ping_ok;
  volatile uint32_t ping_best_us; /* fastest echo reply */
  volatile uint32_t tx_frames;    /* data frames sent */
  volatile uint32_t rx_frames;    /* 802.3 data frames received */
  volatile int32_t conn_rssi;     /* the network's signal in the scan, dBm */
  volatile uint32_t conn_caps;    /* its capability field (0x10: secured) */
  /* 1 while a WIFI_OPT_STAY join is still up for AUDIO_CMD_WIFI_NET. */
  volatile uint32_t session;
  /* WPA2: the network's WIFI_SEC_* and group cipher (4 CCMP, 2 TKIP), the
   * EAPOL-Key frames taken, the RSN element variant message 2 carried, and
   * the keys installed (1 pairwise, 2 group). */
  volatile uint32_t sec;
  volatile uint32_t wpa_group;
  volatile uint32_t wpa_m1, wpa_m3, wpa_g1, wpa_bad_mic;
  volatile uint32_t wpa_variant;
  volatile uint32_t wpa_keys;
  /* The bus in network operations (from core 115): the SDIO clock divider
   * (CLKCTL bits 7:0; 0x20 is the 523 kHz it starts at, 0x01 is 16.8 MHz),
   * 1 while frames are read a block per CMD53 rather than a byte per CMD52,
   * and the commands that failed since the core started. */
  volatile uint32_t bus_div;
  volatile uint32_t bus_rx53;
  volatile uint32_t bus_errs;
} WifiShared;

/* How far joining got (WifiShared.conn_stage). */
enum {
  WIFI_CONN_NONE = 0,
  WIFI_CONN_NOREQ,    /* asked to connect, but no request block           */
  WIFI_CONN_NOTFOUND, /* the scan did not see the network                 */
  WIFI_CONN_SECURED,  /* secured in a way not supported (see sec), or no
                         password given                                   */
  WIFI_CONN_ASSOC,    /* WMI_CONNECT sent, waiting for the firmware       */
  WIFI_CONN_FAILED,   /* DISCONNECT, or no answer, on every attempt       */
  WIFI_CONN_KEYS,     /* WPA2: associated, the 4-way handshake running    */
  WIFI_CONN_BADPASS,  /* WPA2: the handshake failed: a wrong password     */
  WIFI_CONN_LINKED,   /* associated (and keyed): the link is up           */
  WIFI_CONN_DHCP,     /* asking for an address                            */
  WIFI_CONN_NODHCP,   /* no address came                                  */
  WIFI_CONN_IP,       /* the address is ours                              */
  WIFI_CONN_ARP,      /* looking up the gateway's MAC                     */
  WIFI_CONN_PING,     /* pinging the gateway                              */
  WIFI_CONN_NOPING,   /* the gateway did not answer                       */
  WIFI_CONN_DONE,     /* the gateway answered a ping                      */
};

/* What the ARM9 asks the core to join, written before a boot with
 * WIFI_OPT_CONNECT. The password itself never leaves the ARM9: it makes the
 * PMK from it, which stays here and never goes into WifiShared. */
#define WIFI_REQ_ADDR  0x233B8000u
#define WIFI_REQ_MAGIC 0x51524657u /* "WFRQ" */

typedef struct {
  volatile uint32_t magic;
  volatile uint32_t ssid_len;
  volatile uint8_t ssid[32];
  volatile uint32_t has_pmk; /* a password was given */
  volatile uint8_t pmk[32];  /* PBKDF2-SHA1(password, SSID, 4096) */
  volatile uint8_t seed[32]; /* for the SNonce */
} WifiReq;

/* AUDIO_CMD_WIFI_NET's operands and results, on a network a WIFI_OPT_STAY
 * join left up. */
#define WIFI_NET_ADDR 0x233B8100u

enum {
  WIFI_NETOP_STATUS = 1, /* is the link still up                          */
  WIFI_NETOP_DNS,        /* name -> ip, from the DHCP-given DNS server    */
  WIFI_NETOP_PING,       /* one echo to ip with seq; rtt_us, ttl, bytes   */
  WIFI_NETOP_LEAVE,      /* disconnect                                    */
  WIFI_NETOP_HTTP,       /* one HTTP exchange over TCP with ip:port: the
                            request at WIFI_HTTP_REQ goes out, the reply
                            comes back at WIFI_HTTP_RESP until the server
                            closes or its Content-Length is in            */
};

enum {
  WIFI_NETS_OK = 0,
  WIFI_NETS_NOLINK,   /* no session, or the link dropped               */
  WIFI_NETS_TIMEOUT,  /* no answer in time                             */
  WIFI_NETS_NONAME,   /* DNS: the name does not exist (NXDOMAIN)       */
  WIFI_NETS_DNSFAIL,  /* DNS: an answer with no address, or an error   */
  WIFI_NETS_BADNAME,  /* not a valid host name                         */
  WIFI_NETS_NOHOST,   /* a host on our subnet did not answer ARP       */
  WIFI_NETS_ICMPERR,  /* ping: unreachable or time exceeded; see icmp  */
  WIFI_NETS_NOSEND,   /* the frame could not be sent (no credit)       */
  WIFI_NETS_BUSY,     /* ARM9: the core did not take or finish it      */
  WIFI_NETS_REFUSED,  /* HTTP: the server reset the connection (RST)   */
  WIFI_NETS_TOOBIG,   /* HTTP: the request or the reply does not fit   */
};

/* HTTP stages, how far WIFI_NETOP_HTTP got (WifiNetIo.stage). */
enum {
  WIFI_HTTP_CONNECT = 1, /* SYN sent                                      */
  WIFI_HTTP_SEND,        /* connected, the request going out              */
  WIFI_HTTP_WAIT,        /* the request acknowledged, waiting for a reply */
  WIFI_HTTP_DONE,        /* the reply is in                               */
};

/* The request of WIFI_NETOP_HTTP, between WifiNetIo and GpuShared, and its
 * reply, between the core's stack top and AudioCtrl: aShop's downloads come
 * in pieces of up to 512 KB. */
#define WIFI_HTTP_REQ      0x233B9000u
#define WIFI_HTTP_REQ_MAX  0x800u
#define WIFI_HTTP_RESP     0x23200000u
#define WIFI_HTTP_RESP_MAX 0xFF000u

typedef struct {
  volatile uint32_t status;     /* WIFI_NETS_* */
  volatile uint32_t ip;         /* PING: the target; DNS: the answer */
  volatile uint32_t seq;        /* PING */
  volatile uint32_t timeout_ms; /* how long to wait for the answer */
  volatile uint32_t rtt_us;     /* PING: round trip, polling included */
  volatile uint32_t ttl;        /* PING: the reply's TTL */
  volatile uint32_t bytes;      /* PING: ICMP bytes in the reply */
  volatile uint32_t from;       /* PING: who answered */
  volatile uint32_t icmp;       /* PING: type << 8 | code of an ICMP error */
  volatile uint32_t dns_rcode;  /* DNS: the answer's RCODE */
  volatile char name[256];      /* DNS: the host name */
  volatile uint32_t port;       /* HTTP: the server's TCP port */
  volatile uint32_t req_len;    /* HTTP: bytes at WIFI_HTTP_REQ */
  volatile uint32_t resp_len;   /* HTTP: bytes at WIFI_HTTP_RESP */
  volatile uint32_t stage;      /* HTTP: WIFI_HTTP_* */
  volatile uint32_t resent;     /* HTTP: segments sent again */
} WifiNetIo;

/* How far the scan got (WifiShared.wmi_stage). */
enum {
  WIFI_WMI_NONE = 0,
  WIFI_WMI_SETUP,    /* power mode, probed SSID, scan and channel params, filter */
  WIFI_WMI_SCAN,     /* START_SCAN sent                                     */
  WIFI_WMI_DONE,     /* SCAN_COMPLETE came back                             */
  WIFI_WMI_TIMEOUT,  /* the wait for SCAN_COMPLETE ran out                  */
};

/* The controller access in progress (WifiShared.hb), published before each. */
enum {
  WIFI_HB_CMD_WAIT = 1, /* waiting for the command line before a command */
  WIFI_HB_CMD_SENT,     /* a command out, waiting for its response       */
  WIFI_HB_CMD_DONE,
  WIFI_HB_DATA_WAIT,    /* the same three for a data command (CMD53)     */
  WIFI_HB_DATA_SENT,
  WIFI_HB_DATA_FIFO,    /* moving the data through the FIFO              */
  WIFI_HB_DATA_END,     /* data moved, waiting for the transfer to end   */
  WIFI_HB_DATA_DONE,
};

/* Boot options. With all three set the sequence matches the Linux 3DS port
 * (which follows nocash's wifiboot); with none, Octoblimp's ath6kl port. */
#define WIFI_OPT_RESTORE_SOC 0x01u /* put 0x40c4 / 0x180c0 back before BMIDone */
#define WIFI_OPT_NO_POST_LZ  0x02u /* no stub_data/database/HI+0x18 after the LZ */
#define WIFI_OPT_NO_INT_EN   0x04u /* leave INT_STATUS_ENABLE alone (ath6kl zeroes it) */
#define WIFI_OPT_SLEEP_ON    0x08u /* let the chip sleep again at the finish (nocash) */
#define WIFI_OPT_NO_STUB2    0x10u /* after the LZ, skip only the stub_data rewrite */
#define WIFI_OPT_CTRL_ONLY   0x20u /* connect WMI control alone, not the data services */
#define WIFI_OPT_PASSIVE     0x40u /* scan without sending probe requests */
#define WIFI_OPT_CONNECT     0x80u /* then join the WifiReq network, DHCP, ping */
#define WIFI_OPT_STAY        0x100u /* and stay joined, for AUDIO_CMD_WIFI_NET */
#define WIFI_OPT_LINUX       (WIFI_OPT_RESTORE_SOC | WIFI_OPT_NO_POST_LZ | \
                              WIFI_OPT_NO_INT_EN)

enum {
  WIFI_HTC_NONE = 0,
  WIFI_HTC_READY,    /* the firmware posted HTC_READY                     */
  WIFI_HTC_CONNECT,  /* connect-service sent                              */
  WIFI_HTC_CONNECTED,/* the target answered with an endpoint              */
  WIFI_HTC_SETUP,    /* setup-complete sent                               */
  WIFI_HTC_WMI,      /* a WMI event came back                             */
};

enum {
  WIFI_BOOT_NONE = 0,
  WIFI_BOOT_NOFW,   /* firmware not staged in FCRAM (ARM9 SD load failed) */
  WIFI_BOOT_BADVER, /* target version/type is not AR6014, refused to boot */
  WIFI_BOOT_HI,     /* wrote HTC protocol version + SOC clock setup */
  WIFI_BOOT_STUB,   /* uploaded stub_data + stub_code, executed the NWM stub */
  WIFI_BOOT_MAIN,   /* fast-downloaded main firmware + database, set host interest */
  WIFI_BOOT_DONE,   /* BMIDone issued, polled the ready flag */
};

/* ARM9 (WiFi9.c). A command is posted and runs on the core; wifi_done() says
 * when it has finished. */
void wifi_probe(void);
void wifi_boot(uint32_t opts); /* opts: WIFI_OPT_* */
void wifi_get(WifiShared *out); /* Invalidates the cache first. */
int wifi_done(void);
/* 1 while a WIFI_OPT_STAY join is up, as the core last said. */
int wifi_online(void);

/* On the SD card: the firmware Settings > Wi-Fi copies from NAND, and the
 * network it saves ("ssid=" and "password=" lines). */
#define WIFI_FW_DIR   "Aurora/wifi"
#define WIFI_NET_FILE "Aurora/wifi/network.txt"

/* WiFiJoin.c, which apps link too. */
/* Stages the firmware blobs at WIFI_FW_ADDR from the mounted card; 1 when all
 * four were read. */
int wifi_fw_stage(uint32_t main_type);
/* WifiReq for the next boot with WIFI_OPT_CONNECT; pass may be NULL. */
void wifi_request(const char *ssid, const char *pass);
void wifi_request_clear(void);
/* Why a join of `ssid` did not end in a session, as one line. */
void wifi_join_why(const WifiShared *w, const char *ssid, char *out,
                   uint32_t size);

/* While the core runs a Wi-Fi command it cannot present frames, so the
 * screens are drawn straight into the framebuffers on show. wifi_direct_off
 * goes back to presents once the core is done, and returns 1 when it has, or
 * when nothing was running. */
void wifi_direct_on(void);
int wifi_direct_off(void);
/* Waits for the posted command, calling `tick` (may be 0) about every 100 ms
 * with the milliseconds waited; it may draw with the CPU only. 1 when the core
 * finished. */
int wifi_wait(void (*tick)(uint32_t ms), uint32_t max_ms);

/* WifiNetIo's results, as the core left them. */
typedef struct {
  uint32_t status, ip, rtt_us, ttl, bytes, from, icmp, dns_rcode;
} WifiNetResult;

/* One AUDIO_CMD_WIFI_NET (WIFI_NETOP_*) and its wait; returns r->status. */
uint32_t wifi_net(uint32_t op, uint32_t ip, uint32_t seq, const char *name,
                  uint32_t timeout_ms, WifiNetResult *r,
                  void (*tick)(uint32_t ms));

/* One HTTP exchange (WIFI_NETOP_HTTP) with ip:port: `req` goes out as it
 * is, and up to `max` bytes of the reply come back in `resp`, *got of them
 * (with `resp` 0 they stay at WIFI_HTTP_RESP, until the next exchange).
 * timeout_ms bounds the whole exchange (at most 20 s). Returns WIFI_NETS_*;
 * *stage says how far it got. */
uint32_t wifi_http(uint32_t ip, uint32_t port, const char *req,
                   uint32_t req_len, char *resp, uint32_t max, uint32_t *got,
                   uint32_t *stage, uint32_t timeout_ms,
                   void (*tick)(uint32_t ms));

/* os_main.c: joins the network saved in Settings > Wi-Fi and keeps it up for
 * wifi_net. 1 when it is up; otherwise `why` says what stopped it. Staging
 * the firmware mounts the SD card, so the caller must not hold it mounted. */
int wifi_net_join(char *ssid, int ssid_size, char *why, int why_size,
                  void (*tick)(uint32_t ms));

#endif
