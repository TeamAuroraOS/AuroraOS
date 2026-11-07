/* The host harness: a model of the ARM11 core's side of AUDIO_CMD_WIFI_NET
 * (WIFI_NETOP_STATUS, DNS, PING, LEAVE, TCP_BG with the WifiTcp rings, and
 * UDP_BIND and UDP_UNBIND with the WifiUdp rings), scripted servers and UDP
 * peers behind it, and a simulated clock. The model follows what
 * src/os/WiFi11.c does, not how: wifi_net_op, wifi11_tick, tcp_step,
 * udp_step. */
#ifndef HOST_MODEL_H
#define HOST_MODEL_H

#include <stdint.h>

enum {
  SV_ECHO = 1,  /* sends back what it gets */
  SV_HTTP,      /* after "\r\n\r\n", `arg` body bytes, then closes */
  SV_BULK,      /* `arg` bytes as it accepts, then closes */
  SV_REFUSE,    /* resets the SYN */
  SV_SILENT,    /* never answers the SYN */
  SV_RESET,     /* echoes `arg` bytes, then resets */
  SV_HOLD,      /* accepts, then neither sends nor closes */
  SV_SINK,      /* takes `arg` bytes a pass at most, sends nothing */
};

/* Maps 0x23000000..0x24000000 the first time, clears it, and starts a core
 * of `version` on a joined network with the address 192.168.1.50. */
void model_reset(uint32_t version);
/* One pass of the core's loop: a posted command, then the connection. */
void model_step(void);
void model_dns(const char *name, uint32_t ip);
void model_server(uint32_t ip, uint32_t port, int kind, uint32_t arg);
/* Takes the network away (WifiShared.session 0, STATUS answers NOLINK). */
void model_offline(void);
/* Byte i of what SV_BULK and SV_HTTP send. */
uint8_t model_pattern(uint32_t i);

enum {
  UP_ECHO = 1, /* sends each datagram back to where it came from */
  UP_SINK,     /* takes them */
  UP_BLAST,    /* answers the first with `arg` datagrams of 600 bytes */
};
/* A UDP peer, for a core from AUDIO_UDP_VERSION on. 255.255.255.255 is
 * answered by every UP_ECHO peer, as hosts on a LAN would. */
void model_udp_peer(uint32_t ip, uint32_t port, int kind, uint32_t arg);

typedef struct {
  uint32_t ops[16];       /* AUDIO_CMD_WIFI_NET by WIFI_NETOP_* */
  uint32_t bg_stopped;    /* connections another op ended */
  uint32_t fins, aborts;  /* how the ARM9 ended connections */
  uint32_t alive_drops;   /* ended for want of an `alive` bump */
  uint32_t got;           /* bytes servers received, all connections */
  uint32_t connects;
  int bg;                 /* a connection is running */
  uint32_t last_status;   /* WifiNetIo.status the last connection ended with */
  uint32_t udp_sent;      /* datagrams taken from tx and delivered */
  uint32_t udp_to_bcast;  /* of them, to 255.255.255.255 */
  uint32_t udp_binds, udp_unbinds, udp_alive_drops;
  int udp_on;
} ModelStats;
extern ModelStats g_model;

/* The simulated clock, in microseconds. */
uint64_t host_now_us(void);
void host_advance(uint32_t us);

#endif
