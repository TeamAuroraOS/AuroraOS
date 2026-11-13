from pathlib import Path
import subprocess
import tempfile
import sys
root=Path(sys.argv[1]).resolve()
s=(root/'src/os/HomeMenu.c').read_text()
start=s.index('enum { BN_IDLE, BN_WAIT, BN_SHOWN, BN_FAILED };')
end=s.index('static void animate(int ms)', start)
body=s[start:end]
pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;typedef uint32_t u32;
#define HL_NONE -1
#define HL_IS_FOLDER(e) ((e)<-1)
#define TOP_SCREEN_WIDTH 400
#define STATUS_BAR_HEIGHT 24
#define TSH 240
#define ANIM_TOP 1
#define PV_CARD_X 0
#define PV_BOX_Y 0
#define PV_CARD_W 400
#define PV_ALL_H 240
#define PV_BOX_X 0
#define PV_BOX 10
#define PV_CARD_Y 0
#define PV_CARD_H 10
#define PV_HINT_Y 230
static u8 pixels[400*240*3];
#define VRAM_TOP_LA pixels
static int held=-1,bar=-1,selected=0,view,art_shown,by_touch,bottom_valid,dirty;
static struct {const char *path;u32 banner;} apps[3]={{"nova",1},{"plain",0},{"broken",2}};
static int icons,loads,frames,presents,fade_started,current_frame,failed;
static unsigned now;
static int cur(void){return selected;}
static int entry_at(int n){return n;}
static u32 anim_now(void){return now;}
static int anim_ms(u32 t){return now-t;}
static void ui_sound_banner_stop(void){}
static void banner_unload(void){}
static int banner_load(const char *p,u32 at){(void)p;loads++;return at==1;}
static int banner_jingle(u32*a,u32*b,u32*c){(void)a;(void)b;(void)c;return 0;}
static void ui_sound_banner_play(u32 a,u32 b,u32 c){(void)a;(void)b;(void)c;}
static int banner_draw(u32 ms,u32 fade,u32 dst){(void)ms;(void)dst;assert(fade==256);frames++;current_frame=2;return !failed;}
static void screen_touch(volatile u8*p){(void)p;}
static void ui_wallpaper(volatile u8*p,int a,int b,int c){(void)p;(void)a;(void)b;(void)c;current_frame=0;}
static void status_bar_draw(void){}
static void top_item(void){icons++;current_frame=1;}
static void anim_xfade_begin(int s){(void)s;}
static void anim_xfade_area(int s,int x,int y,int w,int h){(void)s;(void)x;(void)y;(void)w;(void)h;}
static void anim_xfade_start(int s,int d){(void)s;(void)d;fade_started++;}
static void screen_present_top(void){assert(current_frame==(apps[selected].banner==1 && !failed?2:1));presents++;}
static void rebuild(void){}
static void place(int n){(void)n;}
static void draw_bottom(void){}
static void screen_present_bottom(void){}
'''
post=r'''
int main(void){
 home_draw();assert(frames==1 && icons==0 && loads==1 && presents==1 && fade_started==0);
 now=16;assert(banner_frame());assert(frames==2 && loads==1);
 selected=1;top_update();assert(icons==1 && fade_started==1);
 selected=0;top_update();assert(frames==3 && icons==1 && loads==1 && fade_started==1);
 banner_leave_home();home_draw();assert(frames==4 && icons==1 && loads==2);
 selected=2;top_update();assert(icons==2 && bn_state==BN_FAILED);
 selected=0;failed=1;top_update();assert(icons==3 && bn_state==BN_FAILED);
 puts("PASS: banner is first presented frame on entry, focus and return; no icon or fade first; failed banners fall back");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'check.c').write_text(pre+body+post)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-Wno-pointer-to-int-cast',str(p/'check.c'),'-o',str(p/'check')],check=True)
 subprocess.run([str(p/'check')],check=True)
