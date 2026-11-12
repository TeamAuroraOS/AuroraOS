"""Compile the exact core TCP transport functions against a deterministic Ethernet peer."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[3]
build=ROOT/'build';build.mkdir(exist_ok=True)
src=(ROOT/'src/os/WiFi11.c').read_text()
start=src.index('static uint32_t tcp_acked(');end=src.index('static void wifi_op_tcp(',start)
preamble=r'''
#define _GNU_SOURCE
#include "wifi.h"
#include "net11.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#define NET_BIT(t) (1u << (t))
static NetState g_net,g_net_extra;
static uint32_t g_tcp_port,g_net_want,now;
static int g_link=1,g_fw_asserted;
static unsigned char frames[128][NET_TX_MAX];static unsigned lengths[128],sent;
static void dcache_clean(void){} static void dcache_clean_inval(void){}
static uint32_t wifi_now(void){return now;}
static int wifi_mac_for(WifiShared*w,uint32_t ip,uint8_t*mac,uint32_t ms){(void)w;(void)ip;(void)ms;memset(mac,2,6);return 1;}
static int wifi_data_send(WifiShared*w,const uint8_t*f,uint32_t n){(void)w;assert(sent<128);memcpy(frames[sent],f,n);lengths[sent++]=n;return 0;}
'''
main=r'''
static NetState clients[2];static unsigned char rx[2][4096];static WifiShared w;static WifiNetIo io;
static WifiTcp *ring(unsigned c){return (WifiTcp *)(uintptr_t)WIFI_TCP_CHANNEL_ADDR(c);}
static NetState *net(unsigned c){return c?&g_net_extra:&g_net;}
static void incoming(unsigned c,const unsigned char*f,unsigned len){unsigned char reply[NET_TX_MAX];unsigned n,v;net_rx(net(c),f,len,reply,&n,&v);if(n)wifi_data_send(&w,reply,n);}
static void deliver(unsigned c,unsigned index){unsigned char reply[NET_TX_MAX];unsigned n,v;net_rx(&clients[c],frames[index],lengths[index],reply,&n,&v);if(n)incoming(c,reply,n);}
static void step(unsigned c){ring(c)->alive++;if(!tcp_step(&w,&io,c))tcp_end(&w,&io,c);}
static void handshake(unsigned c,unsigned port){
 io.ip=0;io.port=port;assert(tcp_begin(&w,&io,15000,c));
 NetState*n=&clients[c];n->ip=0xC0A80105;memset(n->mac,2,6);
 net_tcp_open(n,g_net.ip,40000+c,port,4000+c*100,rx[c],sizeof rx[c]);
 unsigned char syn[NET_TX_MAX];unsigned len=net_tcp_seg(n,syn,g_net.mac,NET_TCP_SYN,n->tcp_iss,NULL,0),index=sent;
 incoming(c,syn,len);assert(sent==index+1);deliver(c,index);step(c);assert(ring(c)->state==WIFI_TCP_OPEN);
}
int main(void){
 for(unsigned c=0;c<2;c++)assert(mmap((void*)(uintptr_t)WIFI_TCP_CHANNEL_ADDR(c),0x10000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=MAP_FAILED);
 g_net.ip=0xC0A80150;memset(g_net.mac,1,6);
 handshake(0,5000);handshake(1,50001);assert(g_net.tcp_lport==5000&&g_net_extra.tcp_lport==50001);
 for(unsigned c=0;c<2;c++){memcpy((void*)ring(c)->tx,c?"data":"ctrl",4);ring(c)->tx_head=4;unsigned index=sent;step(c);assert(sent==index+1);deliver(c,index);step(c);assert(ring(c)->tx_tail==4);assert(clients[c].tcp_rx_len==4);}
 // Dropped data ACK: the exact transport retransmits the channel's own ring.
 memcpy((void*)(ring(1)->tx+4),"retry",5);ring(1)->tx_head=9;unsigned index=sent;step(1);assert(sent==index+1);
 now+=1100000;step(1);assert(sent==index+2);deliver(1,index+1);step(1);assert(ring(1)->tx_tail==9&&ring(0)->tx_tail==4);
 // Incoming data lands in distinct receive rings.
 for(unsigned c=0;c<2;c++){unsigned char f[NET_TX_MAX];unsigned len=net_tcp_seg(&clients[c],f,g_net.mac,NET_TCP_ACK|NET_TCP_PSH,clients[c].snd_nxt,(const unsigned char*)(c?"upload":"command"),c?6:7);clients[c].snd_nxt+=c?6:7;incoming(c,f,len);step(c);assert(ring(c)->rx_head==(c?6:7));}
 // Close the passive data stream without ending the control stream.
 ring(1)->close=WIFI_TCP_FIN;index=sent;step(1);assert(sent==index+1);deliver(1,index);now+=600000;step(1);assert(ring(1)->state==WIFI_TCP_CLOSED&&ring(0)->state==WIFI_TCP_OPEN);
 puts("PASS: exact Aurora core transport, two simultaneous listeners/connections, per-channel tx/rx/ACK rings, dropped-ACK retransmission, independent graceful close");
}
'''
target=build/'tcp-transport-host.c';target.write_text(preamble+src[start:end]+main)
exe=build/'tcp-transport-host'
subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),'-I'+str(ROOT/'src/os'),str(target),str(ROOT/'src/os/Net11.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
