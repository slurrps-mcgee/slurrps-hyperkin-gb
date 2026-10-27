/*
 * The home screen's drawing: one SDL window at 1280x720 logical size,
 * flat rectangles, text and one image at a time.
 *
 * Text is the expensive part on this CPU (FreeType rendering), so every
 * rendered string is kept as a texture and reused; the cache is small and
 * drops the least recently used entry. Everything else is plain rectangles.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include "home.h"
#include "../lib/sqcart.h"

#define FONT_REGULAR	"/usr/share/fonts/dejavu/DejaVuSans.ttf"
#define FONT_BOLD_FILE	"/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"
#define CACHE_SIZE	128
#define TEXT_MAX	160

static SDL_Window* win;
static SDL_Renderer* ren;
static TTF_Font* fonts[FONT_COUNT];

struct cached {
	SDL_Texture* tex;
	int w, h;
	enum font font;
	unsigned color;
	int max_w;		/* the width it was fitted to, 0: none */
	char text[TEXT_MAX];	/* the string asked for (before fitting) */
	Uint32 used;
};
static struct cached cache[CACHE_SIZE];
static Uint32 use_clock;

/* The image shown in the detail pane, kept until another is asked for */
static SDL_Texture* image;
static char image_path[SQ_PATH_LEN];
static int image_w, image_h;

int ui_open(void)
{
	SDL_DisplayMode mode = { 0, UI_W, UI_H, 0, 0 };
	char path[SQ_PATH_LEN];
	static const struct { const char* file; int size; } spec[FONT_COUNT] = {
		{ FONT_REGULAR, 20 },		/* FONT_SMALL: hints, details */
		{ FONT_REGULAR, 24 },		/* FONT_TEXT: lists */
		{ FONT_BOLD_FILE, 24 },		/* FONT_BOLD */
		{ FONT_BOLD_FILE, 32 },		/* FONT_TITLE */
	};
	int i;

	/* Rockchip's SDL picks the HDMI output with this (like sqload) */
	setenv("SDL_MONITOR_INDEX", "1", 0);
	/*
	 * Vsync, with each frame waited for. Without vsync, SDL 2.0.12's KMSDRM
	 * driver queues every page flip but never reads the "flip done" events
	 * the kernel sends back; they pile up in the display device's 4 KB event
	 * buffer, and once it is full (about 128 frames) every flip fails: the
	 * screen stops changing while the program runs on. Frames presented
	 * while a flip was pending were silently dropped too (the "frame shows
	 * only when another follows" effect). With vsync SDL waits for each flip
	 * and reads its event; double buffering makes it wait for the frame
	 * just presented, so it is on screen when ui_present returns.
	 */
	SDL_SetHint("SDL_VIDEO_DOUBLE_BUFFER", "1");	/* SDL_HINT_VIDEO_DOUBLE_BUFFER */
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK) < 0) {
		sq_log("SDL_Init: %s", SDL_GetError());
		return -1;
	}
	SDL_GetDesktopDisplayMode(0, &mode);
	win = SDL_CreateWindow("sqhome", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		mode.w, mode.h, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL);
	if (!win)
		win = SDL_CreateWindow("sqhome", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
			mode.w, mode.h, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN);
	if (!win) {
		sq_log("SDL_CreateWindow: %s", SDL_GetError());
		return -1;
	}
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren)
		ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
	if (!ren) {
		sq_log("SDL_CreateRenderer: %s", SDL_GetError());
		return -1;
	}
	SDL_RenderSetLogicalSize(ren, UI_W, UI_H);
	SDL_ShowCursor(SDL_DISABLE);

	if (TTF_Init() != 0)
		sq_log("TTF_Init: %s", TTF_GetError());
	for (i = 0; i < FONT_COUNT; i++) {
		fonts[i] = TTF_OpenFont(sq_path(path, sizeof(path), spec[i].file), spec[i].size);
		if (!fonts[i])
			sq_log("font %s: %s", path, TTF_GetError());
	}
	IMG_Init(IMG_INIT_PNG);
	return 0;
}

void ui_close(void)
{
	int i;
	for (i = 0; i < CACHE_SIZE; i++)
		if (cache[i].tex)
			SDL_DestroyTexture(cache[i].tex);
	if (image)
		SDL_DestroyTexture(image);
	for (i = 0; i < FONT_COUNT; i++)
		if (fonts[i])
			TTF_CloseFont(fonts[i]);
	if (ren)
		SDL_DestroyRenderer(ren);
	if (win)
		SDL_DestroyWindow(win);
	SDL_Quit();
}

static void set_color(unsigned c)
{
	SDL_SetRenderDrawColor(ren, (c >> 16) & 255, (c >> 8) & 255, c & 255, 255);
}

void ui_clear(void)
{
	set_color(COL_BG);
	SDL_RenderClear(ren);
}

void ui_present(void)
{
	SDL_RenderPresent(ren);
}

void ui_rect(int x, int y, int w, int h, unsigned color)
{
	SDL_Rect r = { x, y, w, h };
	set_color(color);
	SDL_RenderFillRect(ren, &r);
}

void ui_shade(void)
{
	SDL_Rect r = { 0, 0, UI_W, UI_H };
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(ren, 0, 0, 0, 150);
	SDL_RenderFillRect(ren, &r);
	SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
}

/* s shortened to fit max_w (0: no limit), on a UTF-8 character boundary,
 * with "..." added */
static void fit_text(enum font f, const char* s, int max_w, char* buf, size_t len)
{
	size_t n;
	snprintf(buf, len, "%s", s);
	if (max_w <= 0 || ui_text_width(f, buf) <= max_w)
		return;
	n = strlen(buf);
	while (n > 0) {
		do
			n--;
		while (n > 0 && (buf[n] & 0xC0) == 0x80);
		snprintf(buf + n, len - n, "...");
		if (ui_text_width(f, buf) <= max_w)
			break;
	}
}

/* The cached texture for a string drawn at most max_w wide, rendering it on
 * a miss. The key includes max_w, so a hit costs no measuring at all: on
 * this CPU even measuring text every frame adds up. */
static struct cached* get_text(enum font f, unsigned color, const char* s, int max_w)
{
	struct cached* slot = &cache[0];
	SDL_Surface* surf;
	SDL_Color c = { (color >> 16) & 255, (color >> 8) & 255, color & 255, 255 };
	char buf[TEXT_MAX];
	int i;

	for (i = 0; i < CACHE_SIZE; i++) {
		struct cached* e = &cache[i];
		if (e->tex && e->font == f && e->color == color && e->max_w == max_w &&
				!strncmp(e->text, s, TEXT_MAX - 1)) {
			e->used = ++use_clock;
			return e;
		}
		if (!e->tex || e->used < slot->used)
			slot = e;	/* empty or least recently used */
		if (!e->tex)
			break;
	}
	if (!fonts[f])
		return NULL;
	fit_text(f, s, max_w, buf, sizeof(buf));
	surf = TTF_RenderUTF8_Blended(fonts[f], buf, c);
	if (!surf)
		return NULL;
	if (slot->tex)
		SDL_DestroyTexture(slot->tex);
	slot->tex = SDL_CreateTextureFromSurface(ren, surf);
	slot->w = surf->w;
	slot->h = surf->h;
	SDL_FreeSurface(surf);
	slot->font = f;
	slot->color = color;
	slot->max_w = max_w;
	snprintf(slot->text, sizeof(slot->text), "%s", s);
	slot->used = ++use_clock;
	return slot->tex ? slot : NULL;
}

int ui_text_width(enum font f, const char* s)
{
	int w = 0, h;
	if (fonts[f] && *s)
		TTF_SizeUTF8(fonts[f], s, &w, &h);
	return w;
}

static int draw_cached(struct cached* t, int x, int y)
{
	SDL_Rect dst;
	if (!t)
		return 0;
	dst.x = x;
	dst.y = y;
	dst.w = t->w;
	dst.h = t->h;
	SDL_RenderCopy(ren, t->tex, NULL, &dst);
	return t->w;
}

int ui_text(enum font f, int x, int y, unsigned color, const char* s, int max_w)
{
	if (!s || !*s)
		return 0;
	return draw_cached(get_text(f, color, s, max_w), x, y);
}

int ui_text_right(enum font f, int x, int y, unsigned color, const char* s)
{
	struct cached* t;
	if (!s || !*s)
		return 0;
	t = get_text(f, color, s, 0);
	return t ? draw_cached(t, x - t->w, y) : 0;
}

int ui_image(const char* path, int x, int y, int w, int h)
{
	SDL_Rect dst;
	double scale;

	if (!path) {
		if (image)
			SDL_DestroyTexture(image);
		image = NULL;
		image_path[0] = '\0';
		return 0;
	}
	if (strcmp(path, image_path) != 0) {
		SDL_Surface* s;
		if (image)
			SDL_DestroyTexture(image);
		image = NULL;
		snprintf(image_path, sizeof(image_path), "%s", path);
		s = IMG_Load(path);
		if (s) {
			image = SDL_CreateTextureFromSurface(ren, s);
			image_w = s->w;
			image_h = s->h;
			SDL_FreeSurface(s);
		}
	}
	if (!image || image_w <= 0 || image_h <= 0)
		return 0;
	scale = (double)w / image_w < (double)h / image_h ? (double)w / image_w : (double)h / image_h;
	dst.w = (int)(image_w * scale);
	dst.h = (int)(image_h * scale);
	dst.x = x + (w - dst.w) / 2;
	dst.y = y + (h - dst.h) / 2;
	SDL_RenderCopy(ren, image, NULL, &dst);
	return 1;
}

void ui_snapshot(const char* name)
{
	const char* dir = getenv("SQ_HOME_SHOTS");
	char path[SQ_PATH_LEN];
	SDL_Surface* shot;
	int w, h;

	if (!dir)
		return;
	SDL_GetRendererOutputSize(ren, &w, &h);
	shot = SDL_CreateRGBSurface(0, w, h, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0);
	if (!shot)
		return;
	if (SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, shot->pixels, shot->pitch) == 0) {
		snprintf(path, sizeof(path), "%s/%s.bmp", dir, name);
		SDL_SaveBMP(shot, path);
	}
	SDL_FreeSurface(shot);
}
