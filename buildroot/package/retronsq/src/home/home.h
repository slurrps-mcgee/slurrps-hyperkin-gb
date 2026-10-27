/*
 * sqhome, the Retron SQ home screen: what the three files share.
 *
 *   ui.c      the screen: SDL window, cached text, rectangles, images
 *   input.c   the pad (mapped with RetroArch's own autoconfig files) and
 *             the keyboard, as abstract buttons with auto-repeat
 *   home.c    the pages (cartridge, game lists, settings), the cartridge
 *             actions, and handing a game over to sq-session
 *
 * Built to be light on the console's Cortex-A7: it draws only when
 * something changed and sleeps in between.
 */
#ifndef SQHOME_H
#define SQHOME_H

#include <SDL2/SDL.h>

/* --- the screen (ui.c) ---------------------------------------------------- */

#define UI_W 1280	/* everything is laid out in these coordinates, */
#define UI_H 720	/* SDL scales to the real screen */

enum font { FONT_SMALL, FONT_TEXT, FONT_BOLD, FONT_TITLE, FONT_COUNT };

/* Colours, 0xRRGGBB */
#define COL_BG		0x11162a
#define COL_PANEL	0x171d36
#define COL_SIDEBAR	0x141a31
#define COL_ROW		0x26305a
#define COL_ACCENT	0xffb74d
#define COL_TEXT	0xe8eaf6
#define COL_DIM		0x8a90b0
#define COL_DARK	0x11162a
#define COL_OK		0x81c784
#define COL_BAD		0xef7070

/* Open the screen; 0 on success */
int ui_open(void);
void ui_close(void);
void ui_clear(void);
void ui_present(void);
void ui_rect(int x, int y, int w, int h, unsigned color);
/* Darken everything drawn so far (behind a dialog) */
void ui_shade(void);
/* Draw text at x,y (top-left), cut with "..." to max_w (0: no limit).
 * Returns the width drawn. */
int ui_text(enum font f, int x, int y, unsigned color, const char* s, int max_w);
/* Same, right-aligned to x */
int ui_text_right(enum font f, int x, int y, unsigned color, const char* s);
int ui_text_width(enum font f, const char* s);
/* An image (PNG) fitted into the box, centred; NULL path frees the one held.
 * Returns 1 when an image is shown. */
int ui_image(const char* path, int x, int y, int w, int h);
/* Tests: save the screen as <dir>/<name>.bmp when $SQ_HOME_SHOTS is set */
void ui_snapshot(const char* name);

/* --- input (input.c) ------------------------------------------------------- */

enum button { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_X, BTN_Y,
	BTN_L, BTN_R, BTN_START, BTN_SELECT, BTN_COUNT };

void input_open(void);
void input_close(void);
/* Feed one SDL event (pads coming and going, buttons, keys) */
void input_event(const SDL_Event* ev);
/* Press a button (tests, scripts) */
void input_inject(enum button b);
/* Tests: a pretend pad with this USB id, mapped by its autoconfig file;
 * its events come as SDL joystick events with "which" = -100 */
void input_test_pad(int vid, int pid);
/* Auto-repeat held directions; returns how soon (ms) to call again, or
 * -1 when nothing is held */
int input_repeat(Uint32 now);
/* Next pressed button, -1 when none */
int input_next(void);

#endif
