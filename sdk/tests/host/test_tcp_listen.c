#include "net11.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char server_rx[2][4096],client_rx[2][4096];
static NetState server[2],client[2];
static unsigned char smac[6]={2,1,2,3,4,5},cmac[6]={2,6,7,8,9,10};
static int take(NetState*n,const unsigned char*f,unsigned size,unsigned char*reply,unsigned*len) {
 unsigned v;return net_rx(n,f,size,reply,len,&v);
}
int main(void) {
 unsigned char syn[NET_TX_MAX],ack[NET_TX_MAX],reply[NET_TX_MAX],bad[NET_TX_MAX];unsigned an,rn;
 for(unsigned i=0;i<2;i++) {
  memcpy(server[i].mac,smac,6);memcpy(client[i].mac,cmac,6);
  server[i].ip=0xC0A80150;client[i].ip=0xC0A80105;
  net_tcp_open(&server[i],0,5000+i,0,1000+i*100,server_rx[i],sizeof server_rx[i]);server[i].tcp_state=NET_TCP_LISTEN;
  net_tcp_open(&client[i],server[i].ip,40000+i,5000+i,9000+i*100,client_rx[i],sizeof client_rx[i]);
  unsigned size=net_tcp_seg(&client[i],syn,smac,NET_TCP_SYN,client[i].tcp_iss,NULL,0);
  memcpy(bad,syn,size);bad[size-1]^=1;
  take(&server[i],bad,size,reply,&rn);assert(server[i].tcp_state==NET_TCP_LISTEN);
  assert(take(&server[i],syn,size,reply,&rn)==NET_RX_TCP);assert(rn&&server[i].tcp_state==NET_TCP_SYN_RECEIVED);
  unsigned duplicate_len;assert(take(&server[i],syn,size,bad,&duplicate_len)==NET_RX_TCP);assert(duplicate_len==rn&&!memcmp(bad+34,reply+34,rn-34));
  assert(take(&client[i],reply,rn,ack,&an)==NET_RX_TCP);assert(client[i].tcp_state==NET_TCP_OPEN);
  assert(take(&server[i],ack,an,reply,&rn)==NET_RX_TCP);assert(server[i].tcp_state==NET_TCP_OPEN);
  assert(server[i].tcp_ip==client[i].ip&&server[i].tcp_rport==40000+i);
 }
 for(unsigned round=0;round<5;round++)for(unsigned i=0;i<2;i++) {
  const char *text=i?"FTP data\0\xff":"FTP control\r\n";unsigned count=i?10:13;
  unsigned size=net_tcp_seg(&client[i],syn,smac,NET_TCP_ACK|NET_TCP_PSH,client[i].snd_nxt,(const unsigned char*)text,count);
  client[i].snd_nxt+=count;
  assert(take(&server[i],syn,size,reply,&rn)==NET_RX_TCP);
  assert(server[i].tcp_rx_len==(round+1)*count);assert(!memcmp(server_rx[i]+round*count,text,count));
  unsigned duplicate_len;take(&server[i],syn,size,bad,&duplicate_len);assert(server[i].tcp_rx_len==(round+1)*count);
  take(&client[i],reply,rn,ack,&an);assert(client[i].snd_una==client[i].snd_nxt);
  unsigned sn=net_tcp_seg(&server[i],reply,cmac,NET_TCP_ACK|NET_TCP_PSH,server[i].snd_nxt,(const unsigned char*)"OK",2);server[i].snd_nxt+=2;
  take(&client[i],reply,sn,ack,&an);take(&server[i],ack,an,reply,&rn);
  assert(client[i].tcp_rx_len==(round+1)*2);
 }
 unsigned size=net_tcp_seg(&client[1],syn,smac,NET_TCP_ACK|NET_TCP_FIN,client[1].snd_nxt,NULL,0);client[1].snd_nxt++;
 take(&server[1],syn,size,reply,&rn);assert(server[1].tcp_fin&&!server[0].tcp_fin);
 puts("PASS: two independent bidirectional TCP connections, passive handshake, checksum rejection, duplicate SYN/data, acknowledgements, independent FIN");
}
