#ifndef AURORA_APPICON_H
#define AURORA_APPICON_H
#include <stdint.h>
#define APPICON_BYTES 2056u
static inline uint32_t appicon_u32(const unsigned char *p) {
  return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static inline int appicon_header(const unsigned char *h) {
  return h[0]=='A' && h[1]=='I' && h[2]=='C' && h[3]=='1' &&
    h[4]<=1 && !h[5] && !h[6] && !h[7];
}
static inline int appicon_offset(const unsigned char *h, uint32_t size, uint32_t *offset) {
  if (h[0]!='A' || !((h[1]=='U' && h[2]=='R' && (h[3]=='C'||h[3]=='1')) ||
                     (h[1]=='O' && h[2]=='S' && h[3]=='1'))) return 0;
  uint32_t off=appicon_u32(h+4), bytes=appicon_u32(h+8);
  if (off!=36 || bytes<140 || bytes>0x1000000u || size<off || bytes>size-off ||
      size-off-bytes<APPICON_BYTES) return 0;
  *offset=off+bytes;
  return 1;
}
static inline unsigned appicon_alpha(int x,int y,int size,int radius) {
  if (x<0 || y<0 || x>=size || y>=size) return 0;
  if (radius<1) return 255;
  if (radius>size/2) radius=size/2;
  if (x>=size-radius) x=size-1-x;
  if (y>=size-radius) y=size-1-y;
  if (x>=radius || y>=radius) return 255;
  int r=radius*4, hits=0;
  for(int sy=1;sy<4;sy+=2) for(int sx=1;sx<4;sx+=2) {
    int dx=r-(x*4+sx),dy=r-(y*4+sy);
    if(dx*dx+dy*dy<=r*r) hits++;
  }
  return (unsigned)(hits*255+2)/4;
}
#endif
