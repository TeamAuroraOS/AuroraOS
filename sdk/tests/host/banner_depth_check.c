#define _GNU_SOURCE
#include "Banner.c"
#include "ff.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
static FILE *opened;
static unsigned draws;
void os_dcache_flush(void) {}
const u8 *ui_wallpaper_top(void) {static u8 pixels[400*240*3];return pixels;}
int gpu_p3d(uint32_t list,uint32_t bytes,uint32_t dst,uint32_t clear) {
 (void)dst;(void)clear;
 assert(list>=BANNER_ARENA_ADDR && (uint64_t)list+bytes<=BANNER_ARENA_ADDR+BANNER_ARENA_SIZE);
 assert(bytes && !(bytes&7));
 u32 regs[1024]={0};unsigned solid=0,rings=0;int last_pass=-1;
 s32 previous_depth=INT32_MIN;
 const u32 *words=(const u32 *)(uintptr_t)list;
 for(u32 at=0;at<bytes/4;) {
  u32 h=words[at+1],reg=h&1023u,count=((h>>20)&0x7ffu)+1u;
  for(u32 j=0;j<count;j++) {
   u32 r=reg+((h>>31)?j:0),value=words[at+(j?j+1:0)],mask=0;
   for(unsigned k=0;k<4;k++)if(h&(1u<<(16+k)))mask|=255u<<(8*k);
   assert(r<1024);regs[r]=(regs[r]&~mask)|(value&mask);
   if(r==0x22f && value==1) {
    u32 base=bn.base+regs[0x203];
    for(u32 i=0;i<bn.nnode;i++) {
     BnNode *nd=&bn.node[i];if(nd->mesh<0)continue;
     BnMesh *m=&bn.mesh[nd->mesh];
     if(base!=(nd->lit?nd->lit:m->vbase))continue;
     if(is_sprite(nd))break;
     int pass=(regs[0x101]==0x16160000u)?2:(regs[0x107]&0x1000u)?0:1;
     assert(pass>=last_pass);last_pass=pass;
     assert((regs[0x107]&0x71u)==0x61u);
     if(pass==0 && nd->lit && m->tex>=0 && bn.tex[m->tex].translucent) {
      assert(regs[0x104]==(1u|(7u<<4)|(250u<<8)));solid++;
     }
     if(pass==1) {
      s32 z=view_depth(i);assert(z>=previous_depth);previous_depth=z;
     }
     if(pass==2 && nd->mesh==2) {
      assert(solid>=6);assert(!(regs[0x107]&0x1000u));rings++;
     }
     break;
    }
   }
  }
  at+=(count+2u)&~1u;
 }
 assert(solid>=6 && rings==2);
 draws++;return 1;
}
FRESULT f_open(FIL *f,const TCHAR *name,BYTE mode) {
 (void)mode;assert(!opened);opened=fopen(name,"rb");if(!opened)return FR_NO_FILE;
 fseek(opened,0,SEEK_END);f->obj.objsize=ftell(opened);rewind(opened);return FR_OK;
}
FRESULT f_close(FIL *f) {(void)f;fclose(opened);opened=NULL;return FR_OK;}
FRESULT f_lseek(FIL *f,FSIZE_t at) {(void)f;return fseek(opened,at,SEEK_SET)?FR_DISK_ERR:FR_OK;}
FRESULT f_read(FIL *f,void *p,UINT size,UINT *got) {(void)f;*got=fread(p,1,size,opened);return ferror(opened)?FR_DISK_ERR:FR_OK;}
FRESULT f_mount(FATFS *f,const TCHAR *path,BYTE opt) {(void)f;(void)path;(void)opt;return FR_OK;}
int aurora_c_data_dir(const char *path,char *out,unsigned max) {
 size_t n=strlen(path);if(n<4 || n>=max)return 0;memcpy(out,path,n-4);out[n-4]=0;return 1;
}
int main(int argc,char **argv) {
 assert(argc==3);
 assert(mmap((void *)BANNER_ARENA_ADDR,BANNER_ARENA_SIZE,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED_NOREPLACE,-1,0)!=(void *)-1);
 u16 icon[1024];u8 anim;int has;
 u32 where=banner_find(argv[1],icon,&anim,&has);
 assert((where&BANNER_EMBEDDED) && has && anim==1);
 assert(banner_load(argv[1],where));
 assert(banner_node_count()>0);
 u32 addr,samples,rate;
 assert(banner_jingle(&addr,&samples,&rate) && samples && rate);
 printf("PASS: embedded banner loads; %u nodes, jingle %u samples at %u Hz\n",banner_node_count(),samples,rate);
 unsigned times[]={0,350,1500,8000};
 for(unsigned i=0;i<4;i++) {
  s32 matrix[12];assert(banner_node_matrix(0,times[i],matrix));
  assert(banner_draw(times[i],256,0x18300000));
 }
 assert(draws==4);
 puts("PASS: rocket and five cartridge opaque surfaces write depth before both rings; transparent draws sorted far-to-near (four times)");
 FILE *f=fopen(argv[2],"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);u8 *bn=malloc(n);assert(bn && fread(bn,1,n,f)==(size_t)n);fclose(f);
 bn[n-1]^=1;assert(!banner_parse(bn,n));bn[n-1]^=1;bn[4]^=1;assert(!banner_parse(bn,n));free(bn);
 puts("PASS: altered checksum/seal rejected");
 return 0;
}
