#ifndef UI_H
#define UI_H

#include "aurora.h"
#include "assets.h"

/* Each helper uses the asset pack when it loaded and falls back to the built-in
 * icons and 8x8 font otherwise. ui_init() must run before anything draws. */

#define UI_NO_ASSET 0xFFFFFFFFu

extern Font ui_small, ui_font, ui_bold, ui_title;
extern int ui_assets;
extern Color g_accent;

void ui_init(void);

/* Ends one pass of an input loop: collects any GPU blit still in flight, then
 * paces the loop. Every screen loop should call this instead of delay(). */
void ui_idle(void);
int ui_have(const Font *f);

int ui_tw(const Font *f, const char *s);
int ui_th(const Font *f);

/* `y` is the top of the text box with either font, so call sites keep their
 * layout whichever one is in use. */
void ui_text(volatile u8 *fb, int x, int y, int sh, const char *s, Color fg,
             Color bg, const Font *f);
void ui_text_mid(volatile u8 *fb, int cx, int y, int sh, const char *s, Color fg,
                 Color bg, const Font *f);

/* An icon centred in a box-by-box square: pack art at its stored size, else the
 * built-in bitmap scaled to fit. Pass UI_NO_ASSET to force the bitmap. */
void ui_icon(volatile u8 *fb, int bx, int by, int box, int sh, u32 asset,
             const unsigned char *bits, Color tint);

void ui_wallpaper(volatile u8 *fb, int w, int h, int sh);

/* Composes the cached backgrounds, about half a second. Call ui_bg_invalidate()
 * when the accent colour changes. */
void ui_bg_build(void);
void ui_bg_invalidate(void);

/* The same background for one rectangle. Use it to erase a widget before
 * redrawing it, instead of repainting the whole screen. */
void ui_wallpaper_rect(volatile u8 *fb, int x, int y, int w, int h, int sh);

/* Erase just the parts an opaque rounded rect will not cover when redrawn in
 * place: a `border`-wide frame around it, and its own blended corners. */
void ui_patch_round_rect(volatile u8 *fb, int x, int y, int w, int h, int r,
                         int border, int sh);

void ui_dialog(volatile u8 *fb, int w, int h, int sh, const char *title,
               const char *subtitle, Color title_color, int dim);

#endif
