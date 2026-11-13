import sys
import ctypes as c
import subprocess, tempfile, struct
from pathlib import Path
r=Path(__file__).resolve().parents[3]
s=(r/'src/os/HomeMenu.c').read_text()
start=s.index('static unsigned galaxy_phase;')
end=s.index('static void entry_icon16',start)
source='''#include <stdint.h>
#include <string.h>
#include "appicon.h"
typedef uint8_t u8;
typedef uint16_t u16;
typedef struct {u8 r,g,b;} Color;
typedef struct {const u16 *color_icon; unsigned icon_animation;} HomeApp;
static HomeApp apps[1];
static unsigned char screen[400*240*3];
static unsigned width=400,touches,alpha_calls;
#define HL_IS_FOLDER(e) 0
#define BSH 240
#define VRAM_BOT_A screen
static int screen_fb_width(volatile u8 *fb) {(void)fb;return width;}
static void screen_touch(volatile u8 *fb) {(void)fb;touches++;}
static void draw_pixel(volatile u8 *fb,int x,int y,int sh,Color col) {
 unsigned i=(x*sh+sh-1-y)*3;fb[i]=col.b;fb[i+1]=col.g;fb[i+2]=col.r;
}
static void draw_pixel_alpha(volatile u8 *fb,int x,int y,int sh,Color col,unsigned alpha) {
 unsigned i=(x*sh+sh-1-y)*3;
 fb[i]=(fb[i]*(256-alpha)+col.b*alpha)>>8;
 fb[i+1]=(fb[i+1]*(256-alpha)+col.g*alpha)>>8;
 fb[i+2]=(fb[i+2]*(256-alpha)+col.r*alpha)>>8;
}
static unsigned counted_alpha(int x,int y,int size,int radius) {
 alpha_calls++;return appicon_alpha(x,y,size,radius);
}
#define appicon_alpha counted_alpha
'''+s[start:end]+'''
#undef appicon_alpha
'''+'static int color_icon_reference(volatile u8 *fb, int x, int y, int size, int radius, int sh, int e) {\n  if (e < 0 || HL_IS_FOLDER(e) || !apps[e].color_icon) return 0;\n  const HomeApp *app = &apps[e];\n  for (int yy = 0; yy < size; yy++) for (int xx = 0; xx < size; xx++) {\n    int px = x + xx, py = y + yy;\n    if (px < 0 || px >= screen_fb_width(fb) || py < 0 || py >= sh) continue;\n    unsigned alpha=appicon_alpha(xx,yy,size,radius);\n    if (!alpha) continue;\n    unsigned sx = xx * 32 / size, sy = yy * 32 / size;\n    u16 c = app->color_icon[sy * 32 + sx];\n    Color col = {(u8)((c >> 11) * 255 / 31), (u8)(((c >> 5) & 63) * 255 / 63), (u8)((c & 31) * 255 / 31)};\n    if (app->icon_animation) {\n      unsigned wave = (sx + sy + galaxy_phase) & 63;\n      if (wave > 31) wave = 63 - wave;\n      col.r = (u8)(col.r + (255 - col.r) * wave / 180);\n      col.b = (u8)(col.b + (255 - col.b) * wave / 100);\n      if (((sx * 73 + sy * 127) % 193) == 0 && ((galaxy_phase + sx) & 15) < 5)\n        col.r = col.g = col.b = 245;\n    }\n    if(alpha==255) draw_pixel(fb,px,py,sh,col);\n    else draw_pixel_alpha(fb,px,py,sh,col,alpha);\n  }\n  return 1;\n}\n\n'+'''
unsigned alpha(int x,int y,int size,int radius) {return appicon_alpha(x,y,size,radius);}
int offset(const unsigned char *h,unsigned size,unsigned *o) {return appicon_offset(h,size,o);}
int header(const unsigned char *h) {return appicon_header(h);}
unsigned calls(void) {return alpha_calls;}
void invalidate(void) {icon_cache_reset();}
const unsigned char *render_at(const unsigned short *p,int size,int radius,unsigned anim,unsigned phase,int x,int y,unsigned w,int reference) {
 for(unsigned i=0;i<sizeof screen;i++)screen[i]=(i*17+24)&255;
 apps[0]=(HomeApp){p,anim};galaxy_phase=phase;galaxy_dirty=0;width=w;touches=0;
 if(reference)color_icon_reference(screen,x,y,size,radius,240,0);
 else color_icon(screen,x,y,size,radius,240,0);
 return screen;
}
const unsigned char *render(const unsigned short *p,int size,int radius,unsigned anim,unsigned phase) {
 memset(screen,24,sizeof screen);apps[0]=(HomeApp){p,anim};galaxy_phase=phase;width=400;
 color_icon(screen,10,10,size,radius,240,0);return screen;
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'check.c').write_text(source)
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC','-I'+str(r/'include'),str(p/'check.c'),'-o',str(p/'check.so')],check=True)
 api=c.CDLL(str(p/'check.so'));api.offset.argtypes=[c.c_char_p,c.c_uint,c.POINTER(c.c_uint)];api.header.argtypes=[c.c_char_p]
 h=b'AURC'+struct.pack('<8I',36,1024,0,0,0,0,0,0);off=c.c_uint()
 assert api.offset(h,36+1024+2056,c.byref(off)) and off.value==1060
 for size in (0,35,1060,3115):assert not api.offset(h,size,c.byref(off))
 for payload in (0,139,0xFFFFFFFF):
  bad=h[:8]+struct.pack('<I',payload)+h[12:];assert not api.offset(bad,0xFFFFFFFF,c.byref(off))
 assert api.header(b'AIC1\x01\0\0\0') and api.header(b'AIC1\0\0\0\0')
 assert not api.header(b'AIC1\x02\0\0\0') and not api.header(b'AIC1\0\x01\0\0')
 print('PASS: embedded icon header, sizes, overflow and flags')
 for size,radius in ((16,3),(32,6),(48,10),(96,16)):
  vals=[]
  for y in range(size):
   for x in range(size):
    a=api.alpha(x,y,size,radius)
    assert a==api.alpha(size-1-x,y,size,radius)==api.alpha(x,size-1-y,size,radius)
    vals.append(a)
  assert vals[0]==0 and api.alpha(size//2,size//2,size,radius)==255
  assert any(0<a<255 for a in vals)
 print('PASS: rounded masks at all icon sizes, symmetry, empty corners and antialiased edges')
 nova=Path(sys.argv[1]) if len(sys.argv)>1 else r.parent/'NovaLauncher-r18u/NovaLauncher.bin'
 data=nova.read_bytes()
 assert api.offset(data[:36],len(data),c.byref(off))
 block=data[off.value:off.value+2056];assert api.header(block[:8]) and block[4]==1
 pixels=(c.c_uint16*1024).from_buffer_copy(block[8:])
 api.render.argtypes=[c.POINTER(c.c_uint16),c.c_int,c.c_int,c.c_uint,c.c_uint];api.render.restype=c.c_void_p
 a=c.string_at(api.render(pixels,96,16,1,0),400*240*3)
 b=c.string_at(api.render(pixels,96,16,1,12),400*240*3)
 assert a!=b
 for image in (a,b):
  for y in range(240):
   for x in range(400):
    if not(10<=x<106 and 10<=y<106):assert image[(x*240+239-y)*3:(x*240+239-y)*3+3]==bytes([24]*3)
  assert image[(10*240+229)*3:(10*240+229)*3+3]==bytes([24]*3)
 print('PASS: production color renderer clips corners, preserves surrounding pixels and animates embedded Nova artwork')
 api.render_at.argtypes=[c.POINTER(c.c_uint16),c.c_int,c.c_int,c.c_uint,c.c_uint,c.c_int,c.c_int,c.c_uint,c.c_int]
 api.render_at.restype=c.c_void_p
 n=0
 for size in (16,32,*range(46,59),96):
  radius=16 if size==96 else 3 if size==16 else 6 if size==32 else 10+(size-46)//4
  for phase in (0,12,63,64):
   for anim in (0,1):
    for w,x,y in ((320,10,10),(400,365,220),(320,-9,-7),(400,410,250)):
     expected=c.string_at(api.render_at(pixels,size,radius,anim,phase,x,y,w,1),400*240*3)
     actual=c.string_at(api.render_at(pixels,size,radius,anim,phase,x,y,w,0),400*240*3)
     assert expected==actual,(size,phase,anim,w,x,y)
     n+=1
 print(f'PASS: {n} full-buffer comparisons against previous renderer: sizes, animation, both screens and clipping')
 api.render_at(pixels,96,16,1,9,0,0,400,0)
 count=api.calls()
 for i in range(20):api.render_at(pixels,96,16,1,9,0,0,400,0)
 assert api.calls()==count
 pixels[512]^=0xFFFF;api.invalidate()
 actual=c.string_at(api.render_at(pixels,96,16,1,9,0,0,400,0),400*240*3)
 expected=c.string_at(api.render_at(pixels,96,16,1,9,0,0,400,1),400*240*3)
 assert actual==expected
 print('PASS: warm draws reuse masks; reloaded artwork invalidates cached colors')
 try:
  from PIL import Image
  preview=Image.new('RGB',(408,204),(24,24,24))
  for i,image in enumerate((a,b)):
   frame=Image.frombytes('RGB',(240,400),image,'raw','BGR').transpose(Image.Transpose.ROTATE_90).crop((6,6,110,110)).resize((204,204))
   preview.paste(frame,(i*204,0))
  out=r/'build/nova3-icon-preview.png';preview.save(out);print(out)
 except ImportError:pass
