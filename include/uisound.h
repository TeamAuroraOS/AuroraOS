#ifndef AURORA_UISOUND_H
#define AURORA_UISOUND_H

#include "aurora.h"

/* The OS's own sounds from assets.pak (startup, the menu sounds, aShop's music
 * and install chime), decoded into FCRAM nothing else uses, from here to
 * 0x28000000 (the end of Old 3DS FCRAM), and played on app voices, which are
 * free while the OS runs. The Home Menu's song does not fit there; see
 * UiSound.c. */
#define UI_SOUND_ADDR 0x27D80000u

/* The USER_SOUND_* bits of the sounds turned off. */
void ui_sound_config(u8 off);

/* Loads the menu sounds and, when `startup` is set, plays the startup sound.
 * Mounts the card itself. The Home Menu's song follows, from
 * ui_sound_home_tick(). */
void ui_sound_boot(int startup);

/* The Home Menu's song. Call the tick on every pass of the menu's loop: it
 * loads the song a slice at a time and starts it once the startup sound is
 * over. Leave stops it as something opens and enter restarts it from the top
 * on the way back. */
void ui_sound_home_tick(void);
void ui_sound_home_leave(void);
void ui_sound_home_enter(void);
/* Something else used the song's memory (3D Model): load it again. */
void ui_sound_home_lost(void);
/* The lid closed: quiet until it opens and the menu's loop runs again. */
void ui_sound_lid_closed(void);

/* Something chosen with A or a tap, a step back, the selection moving. */
void ui_sound_select(void);
void ui_sound_back(void);
void ui_sound_move(void);
/* For a message that A, B or a tap closed: back for B, else select. */
void ui_sound_close(u32 keys);

/* Loads aShop's music and chime from a card the caller has mounted, and
 * starts the music looping. */
void ui_sound_shop_start(void);
void ui_sound_shop_chime(void);
void ui_sound_shop_stop(void);

#endif
