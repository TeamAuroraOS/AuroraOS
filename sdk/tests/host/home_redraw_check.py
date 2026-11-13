import subprocess,tempfile
from pathlib import Path
r=Path(__file__).resolve().parents[3]
s=(r/'src/os/HomeMenu.c').read_text()
icons=s[s.index('static unsigned galaxy_phase;'):s.index('static void entry_icon16')]
bottom=s[s.index('typedef struct { int x,y,w,h; } HomeRect;'):s.index('/* Points every animation')]
pre=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "appicon.h"
typedef uint8_t u8;
typedef uint16_t u16;
typedef struct {u8 r,g,b;} Color;
typedef struct {const u16 *color_icon;unsigned icon_animation;} HomeApp;
typedef int AnimVal;
#define BW 320
#define BSH 240
#define HOME_PER_PAGE 15
#define SLOT_SIZE 46
#define SLOT_RING 3
#define SLOT_LIFT 3
#define HOLD_LIFT 6
#define DOTS_Y 225
#define HL_NONE -1
#define GAP_ENTRY -1
#define HL_IS_FOLDER(e) ((e)<-1)
#define COLOR_HM_EMPTY_TOP ((Color){23,28,39})
#define COLOR_HM_EMPTY_BOT ((Color){8,10,20})
static Color g_accent={50,80,180};
static u8 screen[BW*BSH*3],expected[BW*BSH*3];
#define VRAM_BOT_A screen
static HomeApp apps[60];
static u16 artwork[60][1024];
static int disp[60],ndisp=30,view,page,bar=-1,held=HL_NONE,into=-1;
static int scroll,ring_x,ring_y,ring_w=58,ring_h=58,held_x,held_y;
static int pos_x[60],pos_y[60],lift[60];
static unsigned full_draws,tiles_drawn;
static int anim_px(const int *p) {return *p;}
static int anim_idx(int e) {return e;}
static int slot_cx(int k) {return (k/15)*BW+21+(k%5)*58;}
static int slot_cy(int k) {return 52+((k%15)/5)*58;}
static int pages(void) {return (ndisp+14)/15;}
static int screen_fb_width(volatile u8 *fb) {(void)fb;return BW;}
static void screen_touch(volatile u8 *fb) {(void)fb;}
static void pixel(volatile u8 *fb,int x,int y,Color c,unsigned a) {
 if(x<0||x>=BW||y<0||y>=BSH)return;
 unsigned o=(x*BSH+BSH-1-y)*3,inv=256-a;
 fb[o]=(fb[o]*inv+c.b*a)>>8;fb[o+1]=(fb[o+1]*inv+c.g*a)>>8;fb[o+2]=(fb[o+2]*inv+c.r*a)>>8;
}
static void draw_gradient_round_rect(volatile u8 *fb,int x,int y,int w,int h,int rad,int sh,Color top,Color bot) {
 (void)sh;
 for(int xx=0;xx<w;xx++)for(int yy=0;yy<h;yy++) {
  Color c={(top.r*(h-yy)+bot.r*yy)/h,(top.g*(h-yy)+bot.g*yy)/h,(top.b*(h-yy)+bot.b*yy)/h};
  unsigned a=appicon_alpha(xx,yy,w,rad);pixel(fb,x+xx,y+yy,c,a==255?256:a);
 }
}
static void draw_round_ring(volatile u8 *fb,int x,int y,int w,int h,int rad,int thick,int sh,Color c) {
 (void)sh;(void)rad;
 for(int xx=0;xx<w;xx++)for(int yy=0;yy<h;yy++)
  if(xx<thick||yy<thick||xx>=w-thick||yy>=h-thick)pixel(fb,x+xx,y+yy,c,190);
}
static void ui_wallpaper(volatile u8 *fb,int w,int h,int sh) {
 (void)sh;full_draws++;
 for(int i=0;i<w*h*3;i++)fb[i]=(i*13+11)%127;
}
static void draw_bar(void) {draw_gradient_round_rect(screen,8,8,304,30,10,BSH,(Color){8,12,18},(Color){18,20,35});}
static void draw_dots(int sc) {
 for(int i=0;i<pages();i++)draw_gradient_round_rect(screen,140+i*12,225,6,6,3,BSH,(Color){30,60,(sc/BW==i)?240:70},(Color){20,50,80});
}
'''
post=r'''
static void tile(volatile u8 *fb,int e,int x,int y,int size) {
 tiles_drawn++;
 int rad=10+(size-SLOT_SIZE)/4;
 draw_gradient_round_rect(fb,x,y,size,size,rad,BSH,(Color){30,40,60},(Color){8,16,32});
 color_icon(fb,x,y,size,rad,BSH,e);
}
'''
main=r'''
int main(void) {
 for(int i=0;i<60;i++) {
  disp[i]=i;pos_x[i]=slot_cx(i);pos_y[i]=slot_cy(i);
  for(int j=0;j<1024;j++)artwork[i][j]=(i*97+j*53)&65535;
  apps[i]=(HomeApp){artwork[i],i==4};
 }
 icon_cache_reset();galaxy_dirty=0;
 ring_x=15;ring_y=46;
 unsigned partial=0,changed_tiles=0,frames=0;
 for(int f=0;f<480;f++) {
  for(int i=0;i<60;i++)lift[i]=0;
  held=HL_NONE;into=-1;bar=-1;
  int selected=(f/8)%15,t=f%8;
  lift[selected]=t<4?t:3;
  ring_x=slot_cx(selected)-6+(t<4?4-t:0);ring_y=slot_cy(selected)-6;
  ring_w=ring_h=58;
  galaxy_phase=f/3;
  page=0;scroll=0;
  if(f>=160&&f<200)scroll=(f-160)*8;
  if(f>=200&&f<240){page=1;scroll=320;}
  if(f>=240&&f<260){held=2;held_x=f-200;held_y=40;into=3;}
  if(f>=280&&f<300){bar=0;ring_x=12;ring_y=6;ring_w=30;ring_h=34;}
  if(f==320){int e=disp[1];disp[1]=disp[5];disp[5]=e;}
  if(f==340)view=1;
  if(f==360){ndisp=15;bottom_valid=0;}
  unsigned before=full_draws;tiles_drawn=0;
  draw_bottom();
  if(full_draws==before){partial++;changed_tiles+=tiles_drawn;}
  memcpy(expected,screen,sizeof screen);
  draw_bottom_full();
  if(memcmp(screen,expected,sizeof screen)) {
   for(unsigned i=0;i<sizeof screen;i++)if(screen[i]!=expected[i]){fprintf(stderr,"mismatch frame %d byte %u: full %u partial %u\n",f,i,screen[i],expected[i]);break;}
   return 1;
  }
  frames++;
 }
 assert(partial>300);
 printf("PASS: %u frame comparisons, including selection, page scrolling, dragging, bar, layout changes and animation\n",frames);
 printf("PASS: %u frames avoid full backdrop redraw; %.2f tiles redrawn per partial frame (15 visible)\n",partial,(double)changed_tiles/partial);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'check.c').write_text(pre+icons+post+bottom+main)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-I'+str(r/'include'),str(p/'check.c'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
