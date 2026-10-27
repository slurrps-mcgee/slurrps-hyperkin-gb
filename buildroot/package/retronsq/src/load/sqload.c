/*
 * sqload: copy the inserted cartridge to the SD card behind the loading
 * screen. Run by PrepareCart (common.sh) the first time a cartridge is
 * played, or when it changed. Exit status 0 means a copy of the cartridge is
 * in the cache afterwards.
 *
 * The copy itself (verified against the reader's CRC, retried once, written
 * atomically) and the save import come from libsqcart; this program only
 * draws progress. The copy starts before the screen is set up, and runs to
 * the end even when there is no screen at all.
 *
 * The slot is a USB 1.1 device (about 1 MB/s), so a big GBA game takes up
 * to ~35 s. The screen says so and shows the time left.
 *
 * Testing aids: SQ_ROOT (sandbox paths, see sqcart.h) and
 * SQ_LOADING_SNAPSHOT=<prefix>, which saves <prefix>-50.bmp and
 * <prefix>-100.bmp screenshots.
 */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>

#include "../lib/sqcart.h"

#define PIC_LOADING	"/etc/retronsq-loading.png"	/* background, see splash/ */
#define FONT_REGULAR	"/usr/share/fonts/dejavu/DejaVuSans.ttf"
#define FONT_BOLD	"/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"
#define MIN_SCREEN_MS	500	/* keep a finished bar up this long */
#ifndef STALL_MS
#define STALL_MS	30000	/* no data from the cartridge this long: give up */
#endif
#define SCREEN_GRACE_MS	10000	/* the screen's time to finish after the copy */

/*
 * Where the live parts go, in the background picture's 1280x720 coordinates
 * (buildroot/package/retronsq/splash/retronsq-loading.svg; keep both in
 * sync). SDL scales everything to the real screen.
 */
#define LOADING_W	1280
#define LOADING_H	720
#define LOADING_NAME_X	470	/* game name, top-left */
#define LOADING_NAME_Y	288
#define LOADING_RIGHT	1092	/* right edge of the percent and time text */
#define LOADING_PCT_Y	372
#define LOADING_BAR_X	472	/* progress bar, inside its track */
#define LOADING_BAR_Y	428
#define LOADING_BAR_W	616
#define LOADING_BAR_H	28
#define LOADING_INFO_X	470	/* "12.0 of 32.0 MB" and "about 20 s left" */
#define LOADING_INFO_Y	476

/* --- the copy (background thread) --------------------------------------- */

static struct sq_cart cart;
static long total_size;			/* ROM + save, for the bar */
static long copied;			/* bytes done, under copy_lock */
static pthread_mutex_t copy_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int copy_finished = 0;
static volatile int copy_ok = 0;
static volatile long activity;		/* bumped on every bit of progress */

/* Milliseconds on a monotonic clock (safe from any thread, before SDL) */
static unsigned long ms_now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000UL + t.tv_nsec / 1000000;
}

static void progress(long done, void* ctx)
{
	(void)ctx;
	pthread_mutex_lock(&copy_lock);
	copied = done;
	pthread_mutex_unlock(&copy_lock);
	activity++;
}

static void* copy_thread(void* arg)
{
	struct sq_error e = { "", 0 };
	int verified;
	unsigned long start = ms_now();
	(void)arg;

	sq_log("copying %s, %ld bytes", cart.rom, cart.rom_size);
	copy_ok = sq_cache_cartridge(&cart, &verified, progress, NULL, &e) == 0;
	if (copy_ok)
		sq_log("copied in %.1f s, %s", (ms_now() - start) / 1000.0,
			verified ? "checksum verified" : "checksum NOT verified (copied anyway)");
	else
		sq_log("copy failed after %.1f s: %s", (ms_now() - start) / 1000.0, e.msg);
	sq_import_save(&cart, &e);

	pthread_mutex_lock(&copy_lock);
	copied = total_size;
	pthread_mutex_unlock(&copy_lock);
	copy_finished = 1;
	return NULL;
}

/* --- the screen --------------------------------------------------------- */

/* A rendered line of text, kept until the text changes (rendering text every
 * frame is too slow for this CPU) */
struct text_cache { char text[128]; SDL_Texture* tex; int w, h; };

static TTF_Font *font_name, *font_pct, *font_info;
static struct text_cache text_name, text_pct, text_size, text_time;
static SDL_Texture* background;

/* Draw text at (x, y); right_align puts its right edge at x */
static void draw_text(SDL_Renderer* r, TTF_Font* font, struct text_cache* c,
	const char* text, int x, int y, SDL_Color color, int right_align)
{
	SDL_Rect dst;
	if (!font)
		return;		/* no fonts: the bar still works */
	if (!c->tex || strcmp(c->text, text) != 0) {
		SDL_Surface* s;
		if (c->tex)
			SDL_DestroyTexture(c->tex);
		c->tex = NULL;
		snprintf(c->text, sizeof(c->text), "%s", text);
		s = TTF_RenderUTF8_Blended(font, text, color);
		if (!s)
			return;
		c->tex = SDL_CreateTextureFromSurface(r, s);
		c->w = s->w;
		c->h = s->h;
		SDL_FreeSurface(s);
	}
	dst.x = right_align ? x - c->w : x;
	dst.y = y;
	dst.w = c->w;
	dst.h = c->h;
	SDL_RenderCopy(r, c->tex, NULL, &dst);
}

/* Fill a rounded bar (a capsule), one horizontal line per row */
static void fill_capsule(SDL_Renderer* r, int x, int y, int w, int h)
{
	double rad = h / 2.0;
	int row;
	for (row = 0; row < h; row++) {
		double dy = row + 0.5 - rad;
		int inset = (int)(rad - sqrt(rad * rad - dy * dy));
		if (x + inset <= x + w - 1 - inset)
			SDL_RenderDrawLine(r, x + inset, y + row, x + w - 1 - inset, y + row);
	}
}

/* One frame: background, game name, bar, percent, MB and time left */
static void draw_loading(SDL_Renderer* r, Uint32 now)
{
	static Uint32 speed_start = 0;
	static long speed_bytes = 0;
	const SDL_Color white = { 255, 255, 255, 255 };
	const SDL_Color accent = { 255, 196, 70, 255 };
	const SDL_Color dim = { 143, 149, 196, 255 };
	char line[128], name[128];
	long done;
	double frac;
	char* p;

	pthread_mutex_lock(&copy_lock);
	done = copied < total_size ? copied : total_size;
	pthread_mutex_unlock(&copy_lock);
	frac = total_size > 0 ? (double)done / total_size : 0;

	SDL_SetRenderDrawColor(r, 11, 16, 38, 255);
	SDL_RenderClear(r);
	if (background)
		SDL_RenderCopy(r, background, NULL, NULL);

	/* "Pokemon_Emerald" -> "Pokemon Emerald" */
	snprintf(name, sizeof(name), "%s", cart.base);
	for (p = name; *p; p++)
		if (*p == '_')
			*p = ' ';
	draw_text(r, font_name, &text_name, name, LOADING_NAME_X, LOADING_NAME_Y, white, 0);

	snprintf(line, sizeof(line), "%d%%", (int)(frac * 100));
	draw_text(r, font_pct, &text_pct, line, LOADING_RIGHT, LOADING_PCT_Y, accent, 1);

	/* The bar, plus a lighter strip along its top for some depth */
	if (done > 0) {
		int w = (int)(LOADING_BAR_W * frac);
		if (w < LOADING_BAR_H)
			w = LOADING_BAR_H;
		SDL_SetRenderDrawColor(r, 255, 160, 60, 255);
		fill_capsule(r, LOADING_BAR_X, LOADING_BAR_Y, w, LOADING_BAR_H);
		SDL_SetRenderDrawColor(r, 255, 210, 110, 255);
		fill_capsule(r, LOADING_BAR_X + 4, LOADING_BAR_Y + 4, w - 8, LOADING_BAR_H / 3);
	}

	snprintf(line, sizeof(line), "%.1f of %.1f MB", done / 1048576.0, total_size / 1048576.0);
	draw_text(r, font_info, &text_size, line, LOADING_INFO_X, LOADING_INFO_Y, dim, 0);

	/* Time left from the measured speed, once 1.5 s of data has arrived */
	if (done > 0 && speed_start == 0) {
		speed_start = now;
		speed_bytes = done;
	}
	if (copy_finished) {
		snprintf(line, sizeof(line), "Done");
	} else if (speed_start && now - speed_start > 1500 && done > speed_bytes) {
		double rate = (done - speed_bytes) * 1000.0 / (now - speed_start);
		int left = (int)((total_size - done) / rate + 0.5);
		if (left >= 60)
			snprintf(line, sizeof(line), "about %d min %02d s left", left / 60, left % 60);
		else
			snprintf(line, sizeof(line), "about %d s left", left < 1 ? 1 : left);
	} else {
		snprintf(line, sizeof(line), "Measuring speed...");
	}
	draw_text(r, font_info, &text_time, line, LOADING_RIGHT, LOADING_INFO_Y, dim, 1);
}

/* With SQ_LOADING_SNAPSHOT=<prefix> set, save the screen as <prefix>-<suffix>.bmp */
static void snapshot(SDL_Renderer* r, const char* suffix)
{
	const char* prefix = getenv("SQ_LOADING_SNAPSHOT");
	char path[SQ_PATH_LEN];
	SDL_Surface* shot;
	int w, h;

	if (!prefix)
		return;
	SDL_GetRendererOutputSize(r, &w, &h);
	shot = SDL_CreateRGBSurface(0, w, h, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0);
	if (!shot)
		return;
	if (SDL_RenderReadPixels(r, NULL, SDL_PIXELFORMAT_ARGB8888, shot->pixels, shot->pitch) == 0) {
		snprintf(path, sizeof(path), "%s-%s.bmp", prefix, suffix);
		SDL_SaveBMP(shot, path);
	}
	SDL_FreeSurface(shot);
}

/* Open window and renderer; any failure just means no loading screen.
 * Creating the window takes the screen from the boot splash, so the slow
 * parts (fonts, decoding the background PNG) happen before it, and the
 * first frame follows the window straight away. */
static SDL_Renderer* open_screen(SDL_Window** win)
{
	SDL_DisplayMode mode = { 0, 1280, 720, 0, 0 };
	SDL_Renderer* r;
	SDL_Surface* picture;
	char path[SQ_PATH_LEN];

	if (TTF_Init() == 0) {
		font_name = TTF_OpenFont(sq_path(path, sizeof(path), FONT_BOLD), 34);
		font_pct = TTF_OpenFont(path, 34);
		font_info = TTF_OpenFont(sq_path(path, sizeof(path), FONT_REGULAR), 22);
	}
	if (!font_name || !font_info)
		sq_log("fonts not found, the loading screen shows the bar only");
	IMG_Init(IMG_INIT_PNG);
	picture = IMG_Load(sq_path(path, sizeof(path), PIC_LOADING));
	if (!picture)
		sq_log("can't load %s: %s", path, IMG_GetError());

	/* Rockchip's SDL picks the HDMI output with this (as the stock loader did) */
	setenv("SDL_MONITOR_INDEX", "1", 0);
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0) {
		sq_log("SDL_Init: %s", SDL_GetError());
		goto fail;
	}
	SDL_GetDesktopDisplayMode(0, &mode);
	*win = SDL_CreateWindow("sqload", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		mode.w, mode.h, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL);
	if (!*win)	/* no GPU: software rendering still shows the screen */
		*win = SDL_CreateWindow("sqload", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
			mode.w, mode.h, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN);
	if (!*win) {
		sq_log("SDL_CreateWindow: %s", SDL_GetError());
		goto fail;
	}
	r = SDL_CreateRenderer(*win, -1, SDL_RENDERER_ACCELERATED);
	if (!r)
		r = SDL_CreateRenderer(*win, -1, SDL_RENDERER_SOFTWARE);
	if (!r) {
		sq_log("SDL_CreateRenderer: %s", SDL_GetError());
		goto fail;
	}
	/* Draw in the background picture's coordinates */
	SDL_RenderSetLogicalSize(r, LOADING_W, LOADING_H);
	if (picture) {
		background = SDL_CreateTextureFromSurface(r, picture);
		SDL_FreeSurface(picture);
	}
	return r;

fail:
	if (picture)
		SDL_FreeSurface(picture);
	return NULL;
}

/*
 * Watchdog, in its own thread so nothing on the screen side can block it.
 * The game must never wait on a hung program:
 *   - no data from the cartridge for STALL_MS: a read stuck in the kernel
 *     can't be interrupted, so leave with a failure (sq-session tries once
 *     more, then shows the menu);
 *   - copy done but the screen (SDL setup, drawing or shutdown) hasn't let
 *     go after SCREEN_GRACE_MS: leave with the copy's result.
 */
static void* watchdog_thread(void* arg)
{
	long last_activity = -1;
	unsigned long last_change = ms_now(), finished_at = 0;
	(void)arg;

	for (;;) {
		usleep(250000);
		if (copy_finished) {
			if (!finished_at)
				finished_at = ms_now();
			else if (ms_now() - finished_at > SCREEN_GRACE_MS) {
				sq_log("the screen didn't finish, leaving anyway");
				_exit(copy_ok ? EXIT_SUCCESS : EXIT_FAILURE);
			}
		} else if (activity != last_activity) {
			last_activity = activity;
			last_change = ms_now();
		} else if (ms_now() - last_change > STALL_MS) {
			sq_log("no data from the cartridge for %d s, giving up", STALL_MS / 1000);
			_exit(EXIT_FAILURE);
		}
	}
	return NULL;
}

int main(void)
{
	pthread_t thread, watchdog;
	SDL_Window* win = NULL;
	SDL_Renderer* r;
	char path[SQ_PATH_LEN];
	int half_saved = 0;
	unsigned long start;

	/* Messages go to launcher.log (PrepareCart): line by line, as they come */
	setvbuf(stderr, NULL, _IOLBF, 0);
	sq_cart_scan(&cart);
	if (!cart.present) {
		sq_log("no cartridge to copy");
		return EXIT_FAILURE;
	}
	/* The save is only copied when there is no live save yet */
	sq_cart_cache_path(&cart, ".srm", path, sizeof(path));
	total_size = cart.rom_size + (sq_file_size(path) < 0 ? cart.save_size : 0);

	/* Start reading the cartridge right away; the screen (EGL, SDL, PNG
	 * decode) gets set up while the first megabytes are already copying */
	if (pthread_create(&thread, NULL, copy_thread, NULL) != 0) {
		sq_log("can't start the copy");
		return EXIT_FAILURE;
	}
	if (pthread_create(&watchdog, NULL, watchdog_thread, NULL) != 0)
		sq_log("can't start the watchdog");

	start = ms_now();
	r = open_screen(&win);
	sq_log(r ? "loading screen up after %lu ms" : "no loading screen (%lu ms), copying anyway",
		ms_now() - start);

	/* Draw until the copy is done (the watchdog handles a stuck copy) */
	while (!copy_finished) {
		if (r) {
			SDL_Event ev;
			while (SDL_PollEvent(&ev))
				;	/* keep the window responsive; nothing to handle */
			draw_loading(r, SDL_GetTicks());
			if (!half_saved && total_size > 0 && copied * 2 >= total_size) {
				snapshot(r, "50");
				half_saved = 1;
			}
			SDL_RenderPresent(r);
		}
		usleep(16000);
	}
	if (r) {
		draw_loading(r, SDL_GetTicks());
		snapshot(r, "100");
		SDL_RenderPresent(r);
		if (ms_now() - start < MIN_SCREEN_MS)
			SDL_Delay(MIN_SCREEN_MS - (ms_now() - start));
	}
	pthread_join(thread, NULL);

	/* The copy is done and its result final. Shutting SDL down hands the
	 * screen back; the watchdog makes sure it can't keep the game waiting. */
	if (font_name) TTF_CloseFont(font_name);
	if (font_pct) TTF_CloseFont(font_pct);
	if (font_info) TTF_CloseFont(font_info);
	if (r) SDL_DestroyRenderer(r);
	if (win) SDL_DestroyWindow(win);
	SDL_Quit();
	sq_log("done");
	return copy_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
