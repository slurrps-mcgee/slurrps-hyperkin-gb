/*
 * Cartridge Tools: a small libretro "core" that adds cartridge actions to
 * RetroArch's menu (Load Content > Collections > Cartridge Tools). It will
 * move into the home UI later; until then it is how these are reached.
 *
 * RetroArch 1.7.4 has no way to add menu entries without patching it, so this
 * runs like a game: RetroArch loads it, and it draws its own menu with
 * RetroArch's RGUI font and reads the pad. It offers:
 *
 *   Play cartridge           leave RetroArch; sq-session then starts the
 *                            cartridge (also with auto-load off)
 *   Copy cartridge to SD     ROM -> roms/<system>/, checked against the
 *                            reader's CRC, plus its save
 *   Back up cartridge save   cartridge save -> saves/<game>/NNN-cartridge.sav
 *   Restore a save...        pick a backup (or the save on the cartridge) to
 *                            become rom/<game>.srm, the save used when the
 *                            cartridge is played
 *   Write a save to the cartridge...
 *                            experimental: pick the live save or a backup
 *                            and write it onto the cartridge itself
 *
 * All cartridge work is done by libsqcart (lib/sqcart.h); this file is only
 * the screens. The cartridge is read-only (mounted read-only, its device
 * locked) except during "Write a save to the cartridge": that backs up the
 * cartridge's save, unlocks it (cart-event write-enable), writes the one
 * save file in place and checks it, and locks it again (write-disable).
 * Long copies run in a worker thread so the progress bar keeps moving, and
 * every action re-checks that the cartridge wasn't swapped meanwhile.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "libretro.h"
#include "bitmap.h"		/* RetroArch's 5x10 RGUI font (GPLv3) */
#include "../lib/sqcart.h"

#define PLAY_FLAG	SQ_RUN_DIR "/play-cart"		/* see sq-session */
#define FAILED_FLAG	SQ_RUN_DIR "/cart-failed"	/* see sq-session */
#ifndef SCAN_CMD
#define SCAN_CMD	"/usr/lib/retronsq/sq-scan-roms"	/* refreshes Collections */
#endif
/* Opens / closes the cartridge for writing (see cart-event). Tests replace it. */
#ifndef CART_RW_CMD
#define CART_RW_CMD	"/usr/lib/retronsq/cart-event"
#endif
#define VERSION_FILE	"/etc/retronsq-version"		/* "Retron SQ CFW v0.3.2 beta" */

/* Leave RetroArch so sq-session takes over. SIGTERM makes RetroArch exit
 * cleanly (saving), like sq-session's own stop. Tests replace it. */
#ifndef QUIT_RETROARCH
#define QUIT_RETROARCH() kill(getpid(), SIGTERM)
#endif

#define WIDTH	320
#define HEIGHT	240
#define MARGIN	8
#define LINE	FONT_HEIGHT_STRIDE			/* 11 pixels */
#define COLS	((WIDTH - 2 * MARGIN) / FONT_WIDTH_STRIDE)	/* 50 characters */
#define MAX_BACKUPS 64

#define RGB565(r, g, b) (uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
#define C_BG		RGB565(24, 28, 40)
#define C_TEXT		RGB565(230, 230, 230)
#define C_DIM		RGB565(110, 110, 120)
#define C_TITLE		RGB565(255, 200, 60)
#define C_SELECT	RGB565(60, 80, 140)
#define C_OK		RGB565(120, 220, 120)
#define C_BAD		RGB565(255, 110, 110)

/* --- libretro callbacks ------------------------------------------------- */

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static uint16_t fb[WIDTH * HEIGHT];		/* the screen, RGB565 */
static int16_t silence[735 * 2];		/* one frame of 44.1 kHz stereo */

/* --- state -------------------------------------------------------------- */

static struct sq_cart cart;			/* the inserted cartridge */

enum screen { SCREEN_MAIN, SCREEN_RESTORE, SCREEN_CONFIRM, SCREEN_BUSY, SCREEN_MESSAGE };
static enum screen screen = SCREEN_MAIN;

enum item { ITEM_PLAY, ITEM_DUMP_ROM, ITEM_DUMP_SAVE, ITEM_RESTORE, ITEM_WRITE_SAVE,
	ITEM_EXIT, ITEM_COUNT };
static const char* item_label[ITEM_COUNT] = {
	"Play cartridge",
	"Copy cartridge to SD card",
	"Back up cartridge save",
	"Restore a save...",
	"Write a save to the cartridge...",
	"Back to RetroArch menu",
};
static int main_sel = 0;

/* Saves offered by "Restore a save..." or "Write a save to the
 * cartridge..." (sq_save_choices) */
static struct sq_save_choice backups[MAX_BACKUPS + 1];
static int backup_count = 0, restore_sel = 0;
static int writing = 0;		/* the list is for writing to the cartridge */
static int confirm_step = 0;	/* writing asks twice */
static int confirm_button = 0;	/* 0 = No (the default), 1 = Yes */

/* Result shown on SCREEN_MESSAGE: up to 4 lines, the first one coloured */
static char msg[4][COLS + 1];
static uint16_t msg_color = C_TEXT;

/* Worker thread state. work_done/work_total drive the progress bar. */
static pthread_t worker;
static volatile int worker_busy = 0;
static volatile long work_done = 0, work_total = 0;
static char work_title[COLS + 1];
static struct sq_meter meter;		/* "MB of MB", speed and time left */

static int frame_count = 0;
static char fw_version[COLS + 1];	/* "v0.3.2 beta", shown in the header */

/* --- small helpers ------------------------------------------------------ */

/* Set the result screen's text; lines past the first are optional (NULL) */
static void SetMessage(uint16_t color, const char* l1, const char* l2, const char* l3, const char* l4)
{
	const char* lines[4] = { l1, l2, l3, l4 };
	int i;
	for (i = 0; i < 4; i++)
		snprintf(msg[i], sizeof(msg[i]), "%s", lines[i] ? lines[i] : "");
	msg_color = color;
}

/* Show what a shared action (sqcart.h, sq_do_*) did */
static void SetResult(const struct sq_result* r)
{
	SetMessage(r->ok ? C_OK : C_BAD, r->line[0], r->line[1], r->line[2], r->line[3]);
}

/* "128 KB", "16.0 MB" */
static void SizeText(long size, char* out, size_t len)
{
	if (size >= 1024 * 1024)
		snprintf(out, len, "%.1f MB", size / (1024.0 * 1024.0));
	else
		snprintf(out, len, "%ld KB", (size + 1023) / 1024);
}

/* Milliseconds on a monotonic clock, for the copy's speed */
static unsigned long NowMs(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000UL + t.tv_nsec / 1000000;
}

static void Progress(long done, void* ctx)
{
	(void)ctx;
	work_done = done;
}

/* Fill backups[] for the restore or write list */
static void ListBackups(void)
{
	backup_count = sq_save_choices(&cart, writing, backups, MAX_BACKUPS + 1);
	restore_sel = 0;
}

/* --- the actions (run in the worker thread) ----------------------------- */

/* The actions are libsqcart's (sq_do_*, shared with the home screen); these
 * only run them in the worker thread and show the result */

/* Copy cartridge to SD card, then refresh the Collections */
static void* TaskDumpRom(void* arg)
{
	struct sq_result r;
	int added;
	(void)arg;

	snprintf(work_title, sizeof(work_title), "Copying %s...", cart.rom);
	work_total = cart.rom_size;
	sq_do_copy_to_library(&cart, Progress, NULL, &added, &r);
	/* Make it show up under Load Content > Collections right away */
	if (added && system(SCAN_CMD " > /dev/null 2>&1") != 0)
		sq_log("%s failed", SCAN_CMD);
	SetResult(&r);
	worker_busy = 0;
	return NULL;
}

static void* TaskDumpSave(void* arg)
{
	struct sq_result r;
	(void)arg;
	snprintf(work_title, sizeof(work_title), "Backing up the save...");
	work_total = cart.save_size;
	sq_do_backup_save(&cart, Progress, NULL, &r);
	SetResult(&r);
	worker_busy = 0;
	return NULL;
}

static void* TaskWriteSave(void* arg)
{
	struct sq_result r;
	(void)arg;
	snprintf(work_title, sizeof(work_title), "Writing the save to the cartridge...");
	work_total = 3 * cart.save_size;	/* backup, write, read back */
	sq_do_write_save(&cart, backups[restore_sel].path, CART_RW_CMD, Progress, NULL, &r);
	SetResult(&r);
	worker_busy = 0;
	return NULL;
}

static void* TaskRestore(void* arg)
{
	struct sq_result r;
	(void)arg;
	snprintf(work_title, sizeof(work_title), "Restoring the save...");
	work_total = sq_file_size(backups[restore_sel].path);
	sq_do_restore_save(&cart, backups[restore_sel].path, Progress, NULL, &r);
	SetResult(&r);
	worker_busy = 0;
	return NULL;
}

static void StartTask(void* (*task)(void*))
{
	/* A swapped or pulled cartridge: say so instead of failing midway */
	if (!sq_cart_still_there(&cart)) {
		sq_cart_scan(&cart);
		SetMessage(C_BAD, "The cartridge was removed or changed.",
			"Check it and try again.", NULL, NULL);
		screen = SCREEN_MESSAGE;
		return;
	}
	work_done = work_total = 0;
	meter.total = -1;	/* a new meter once the task says its size */
	work_title[0] = '\0';
	worker_busy = 1;
	screen = SCREEN_BUSY;
	if (pthread_create(&worker, NULL, task, NULL) != 0) {
		worker_busy = 0;
		SetMessage(C_BAD, "Couldn't start the task.", NULL, NULL, NULL);
		screen = SCREEN_MESSAGE;
	}
}

/* --- drawing ------------------------------------------------------------ */

static void FillRect(int x, int y, int w, int h, uint16_t color)
{
	int i, j;
	for (j = y; j < y + h && j < HEIGHT; j++)
		for (i = x; i < x + w && i < WIDTH; i++)
			if (i >= 0 && j >= 0)
				fb[j * WIDTH + i] = color;
}

/* Draw text with the RGUI font, cut at the right margin. Each glyph is
 * FONT_WIDTH x FONT_HEIGHT bits, packed row by row. */
static void DrawText(int x, int y, const char* text, uint16_t color)
{
	int i, j;
	if (y < 0 || y + FONT_HEIGHT > HEIGHT)
		return;
	for (; *text && x + FONT_WIDTH <= WIDTH - MARGIN / 2; text++, x += FONT_WIDTH_STRIDE) {
		unsigned char symbol = (unsigned char)*text;
		for (j = 0; j < FONT_HEIGHT; j++) {
			for (i = 0; i < FONT_WIDTH; i++) {
				uint8_t rem = 1 << ((i + j * FONT_WIDTH) & 7);
				int offset = (i + j * FONT_WIDTH) >> 3;
				if (bitmap_bin[FONT_OFFSET(symbol) + offset] & rem)
					fb[(y + j) * WIDTH + x + i] = color;
			}
		}
	}
}

static int ItemEnabled(int item)
{
	switch (item) {
	case ITEM_PLAY:
	case ITEM_DUMP_ROM:
		return cart.present;
	case ITEM_DUMP_SAVE:
		return cart.present && cart.save_size > 0;
	case ITEM_RESTORE:
		return cart.present;
	case ITEM_WRITE_SAVE:
		/* Not when the slot reports write protection */
		return cart.present && cart.save_size > 0 && sq_reader_writable() != 0;
	default:
		return 1;
	}
}

static const char* ItemHelp(int item)
{
	switch (item) {
	case ITEM_PLAY:      return "Start the cartridge in the slot.";
	case ITEM_DUMP_ROM:  return "Put the game in roms/ to play without the cartridge.";
	case ITEM_DUMP_SAVE: return "Copy the cartridge's save into saves/ on the SD card.";
	case ITEM_RESTORE:   return "Choose which save to use with this cartridge.";
	case ITEM_WRITE_SAVE:
		return cart.present && sq_reader_writable() == 0
			? "Not available: the slot is write protected."
			: "Experimental: put a save on the cartridge.";
	default:             return "Return to RetroArch's menu.";
	}
}

/* Title and cartridge details, shared by every screen */
static int DrawHeader(void)
{
	char line[COLS + 1], rom_size[16], save_size[16];
	int y = MARGIN;

	DrawText(MARGIN, y, "CARTRIDGE TOOLS", C_TITLE);
	DrawText(WIDTH - MARGIN - (int)strlen(fw_version) * FONT_WIDTH_STRIDE, y, fw_version, C_DIM);
	y += LINE;
	FillRect(MARGIN, y, WIDTH - 2 * MARGIN, 1, C_DIM);
	y += LINE / 2;
	if (cart.present) {
		SizeText(cart.rom_size, rom_size, sizeof(rom_size));
		if (cart.save_size > 0)
			SizeText(cart.save_size, save_size, sizeof(save_size));
		else
			snprintf(save_size, sizeof(save_size), "none");
		snprintf(line, sizeof(line), "Cartridge: %s", cart.rom);
		DrawText(MARGIN, y, line, C_TEXT);
		y += LINE;
		snprintf(line, sizeof(line), "%s, %s, save: %s", cart.system, rom_size, save_size);
		DrawText(MARGIN, y, line, C_DIM);
	} else {
		DrawText(MARGIN, y, "No cartridge inserted.", C_TEXT);
		y += LINE;
		DrawText(MARGIN, y, "Insert one: it starts automatically.", C_DIM);
	}
	return y + 2 * LINE;
}

/* Buttons in a row above the footer, the one at sel highlighted */
static void DrawButtons(const char* const* labels, int n, int sel)
{
	int i, x = MARGIN, y = HEIGHT - 3 * LINE;
	for (i = 0; i < n; i++) {
		int w = ((int)strlen(labels[i]) + 4) * FONT_WIDTH_STRIDE;
		FillRect(x, y - 3, w, LINE + 4, i == sel ? C_SELECT : C_DIM);
		DrawText(x + 2 * FONT_WIDTH_STRIDE, y, labels[i], i == sel ? C_TITLE : C_TEXT);
		x += w + 8;
	}
}

static void DrawFooter(const char* text)
{
	FillRect(MARGIN, HEIGHT - MARGIN - LINE - 3, WIDTH - 2 * MARGIN, 1, C_DIM);
	DrawText(MARGIN, HEIGHT - MARGIN - LINE, text, C_DIM);
}

/* A list with a highlight bar on the selected entry */
static void DrawList(int y, const char** labels, int count, int sel, int (*enabled)(int))
{
	int i;
	for (i = 0; i < count; i++, y += LINE + 2) {
		int on = !enabled || enabled(i);
		if (i == sel)
			FillRect(MARGIN - 2, y - 2, WIDTH - 2 * MARGIN + 4, LINE + 2, C_SELECT);
		DrawText(MARGIN + 4, y, labels[i], on ? C_TEXT : C_DIM);
	}
}

static void Render(void)
{
	static const char* const yes_no[2] = { "No", "Yes" };
	static const char* const no_write[2] = { "No", "Write it" };
	static const char* const ok[1] = { "OK" };
	int y, i;
	const char* labels[MAX_BACKUPS + 1];

	FillRect(0, 0, WIDTH, HEIGHT, C_BG);
	y = DrawHeader();

	switch (screen) {
	case SCREEN_MAIN:
		DrawList(y, item_label, ITEM_COUNT, main_sel, ItemEnabled);
		y += ITEM_COUNT * (LINE + 2) + LINE / 2;
		DrawText(MARGIN, y, ItemHelp(main_sel), C_DIM);
		DrawFooter("A: select     Start+Select: RetroArch menu");
		break;

	case SCREEN_RESTORE:
		DrawText(MARGIN, y, writing ? "Write which save to the cartridge?"
			: "Use which save with this cartridge?", C_TEXT);
		y += LINE + LINE / 2;
		if (backup_count == 0) {
			DrawText(MARGIN, y, "No saves found for this game:", C_DIM);
			DrawText(MARGIN, y + LINE, writing ? "play it or back up its save first."
				: "back up the cartridge's save first.", C_DIM);
		} else {
			/* Scroll so the selection stays on screen (9 rows fit) */
			int first = restore_sel > 8 ? restore_sel - 8 : 0;
			int shown = backup_count - first < 9 ? backup_count - first : 9;
			for (i = 0; i < shown; i++)
				labels[i] = backups[first + i].label;
			DrawList(y, labels, shown, restore_sel - first, NULL);
		}
		DrawFooter("A: choose     B: back");
		break;

	case SCREEN_CONFIRM:
		if (writing) {
			DrawText(MARGIN, y, confirm_step == 0 ? "Write this save onto the CARTRIDGE?"
				: "Sure? This changes the cartridge itself.", confirm_step ? C_BAD : C_TEXT);
			y += LINE + LINE / 2;
			DrawText(MARGIN + 8, y, backups[restore_sel].label, C_TITLE);
			y += 2 * LINE;
			DrawText(MARGIN, y, "The cartridge's save is backed up to the", C_DIM);
			y += LINE;
			DrawText(MARGIN, y, "SD card first. Experimental: not every", C_DIM);
			y += LINE;
			DrawText(MARGIN, y, "cartridge slot can write. Don't remove", C_DIM);
			y += LINE;
			DrawText(MARGIN, y, "the cartridge until it's done.", C_DIM);
			DrawButtons(confirm_step == 0 ? yes_no : no_write, 2, confirm_button);
			DrawFooter("Left/Right: choose   A: press   B: no");
			break;
		}
		DrawText(MARGIN, y, "Use this save when playing the cartridge?", C_TEXT);
		y += LINE + LINE / 2;
		DrawText(MARGIN + 8, y, backups[restore_sel].label, C_TITLE);
		y += 2 * LINE;
		DrawText(MARGIN, y, "Your current save is backed up first.", C_DIM);
		y += LINE;
		DrawText(MARGIN, y, "The cartridge itself isn't changed.", C_DIM);
		DrawButtons(yes_no, 2, confirm_button);
		DrawFooter("Left/Right: choose   A: press   B: no");
		break;

	case SCREEN_BUSY:
		DrawText(MARGIN, y, work_title, C_TEXT);
		y += 2 * LINE;
		if (work_total > 0) {
			/* Bar, "MB of MB" (KB for saves), speed and time left, the
			 * same words as the loading screen and the home screen */
			char size[COLS + 1], rate[COLS + 1];
			int w = WIDTH - 2 * MARGIN;
			if (meter.total != work_total)
				sq_meter_start(&meter, work_total);
			FillRect(MARGIN, y, w, LINE, C_DIM);
			FillRect(MARGIN, y, (int)((double)w * work_done / work_total), LINE, C_TITLE);
			sq_meter_text(&meter, work_done, NowMs(), 0, size, sizeof(size), rate, sizeof(rate));
			y += LINE + LINE / 2;
			DrawText(MARGIN, y, size, C_TEXT);
			y += LINE;
			DrawText(MARGIN, y, rate, C_TEXT);
		} else {
			/* No size known: a dot moving back and forth */
			int pos = (frame_count / 4) % 20;
			DrawText(MARGIN + (pos < 10 ? pos : 20 - pos) * FONT_WIDTH_STRIDE, y, "*", C_TITLE);
		}
		y += 2 * LINE;
		DrawText(MARGIN, y, "Please don't remove the cartridge.", C_DIM);
		DrawFooter("Working...");
		break;

	case SCREEN_MESSAGE:
		DrawText(MARGIN, y, msg[0], msg_color);
		for (i = 1; i < 4; i++)
			DrawText(MARGIN, y + i * LINE + LINE / 2, msg[i], C_TEXT);
		DrawButtons(ok, 1, 0);
		DrawFooter("A: OK");
		break;
	}
}

/* --- input -------------------------------------------------------------- */

static int16_t prev_pad[16];

/* Buttons that went down this frame (pressed, not held) */
static unsigned ReadPresses(void)
{
	unsigned pressed = 0, id;
	input_poll_cb();
	for (id = 0; id < 16; id++) {
		int16_t now = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id);
		if (now && !prev_pad[id])
			pressed |= 1u << id;
		prev_pad[id] = now;
	}
	return pressed;
}

#define PRESSED(id) (pressed & (1u << RETRO_DEVICE_ID_JOYPAD_##id))

static void HandleInput(unsigned pressed)
{
	switch (screen) {
	case SCREEN_MAIN:
		if (PRESSED(UP))
			main_sel = (main_sel + ITEM_COUNT - 1) % ITEM_COUNT;
		if (PRESSED(DOWN))
			main_sel = (main_sel + 1) % ITEM_COUNT;
		if (!PRESSED(A) || !ItemEnabled(main_sel))
			break;
		switch (main_sel) {
		case ITEM_PLAY: {
			/* Ask sq-session for the cartridge (works with auto-load off)
			 * and clear a "failed to load" mark so it tries again */
			char path[SQ_PATH_LEN];
			FILE* fp;
			unlink(sq_path(path, sizeof(path), FAILED_FLAG));
			fp = fopen(sq_path(path, sizeof(path), PLAY_FLAG), "w");
			if (fp)
				fclose(fp);
			SetMessage(C_OK, "Starting the cartridge...", NULL, NULL, NULL);
			screen = SCREEN_MESSAGE;
			QUIT_RETROARCH();
			break;
		}
		case ITEM_DUMP_ROM:
			StartTask(TaskDumpRom);
			break;
		case ITEM_DUMP_SAVE:
			StartTask(TaskDumpSave);
			break;
		case ITEM_RESTORE:
		case ITEM_WRITE_SAVE:
			writing = main_sel == ITEM_WRITE_SAVE;
			ListBackups();
			screen = SCREEN_RESTORE;
			break;
		case ITEM_EXIT:
			environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
			break;
		}
		break;

	case SCREEN_RESTORE:
		if (PRESSED(B) || (PRESSED(A) && backup_count == 0))
			screen = SCREEN_MAIN;
		else if (PRESSED(UP) && restore_sel > 0)
			restore_sel--;
		else if (PRESSED(DOWN) && restore_sel < backup_count - 1)
			restore_sel++;
		else if (PRESSED(A)) {
			confirm_step = 0;
			confirm_button = 0;
			screen = SCREEN_CONFIRM;
		}
		break;

	case SCREEN_CONFIRM:
		/* No and Yes buttons: Left/Right pick, A presses, B is No */
		if (PRESSED(LEFT) || PRESSED(RIGHT) || PRESSED(UP) || PRESSED(DOWN))
			confirm_button = !confirm_button;
		else if (PRESSED(B) || (PRESSED(A) && confirm_button == 0))
			screen = SCREEN_RESTORE;
		else if (PRESSED(A) && writing && confirm_step == 0) {
			confirm_step = 1;	/* writing to a cartridge: ask again */
			confirm_button = 0;
		} else if (PRESSED(A))
			StartTask(writing ? TaskWriteSave : TaskRestore);
		break;

	case SCREEN_BUSY:
		break;

	case SCREEN_MESSAGE:
		if (PRESSED(A) || PRESSED(B)) {
			sq_cart_scan(&cart);
			screen = SCREEN_MAIN;
		}
		break;
	}
}

/* --- libretro API ------------------------------------------------------- */

void retro_set_environment(retro_environment_t cb)
{
	bool no_game = true;
	environ_cb = cb;
	/* Can also be started from Load Core without any content */
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info* info)
{
	memset(info, 0, sizeof(*info));
	info->library_name = "Cartridge Tools";
	info->library_version = "1.0";
	info->valid_extensions = "sqt";	/* the tools' launcher file */
	info->need_fullpath = true;
	info->block_extract = true;
}

void retro_get_system_av_info(struct retro_system_av_info* info)
{
	memset(info, 0, sizeof(*info));
	info->geometry.base_width = info->geometry.max_width = WIDTH;
	info->geometry.base_height = info->geometry.max_height = HEIGHT;
	info->geometry.aspect_ratio = 4.0f / 3.0f;
	info->timing.fps = 60.0;
	info->timing.sample_rate = 44100.0;
}

void retro_init(void)
{
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
	char line[128] = "", path[SQ_PATH_LEN];
	const char* v;
	FILE* fp;

	environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);

	/* "Retron SQ CFW v0.1.0 beta" -> "v0.1.0 beta" */
	fp = fopen(sq_path(path, sizeof(path), VERSION_FILE), "r");
	if (fp) {
		if (fgets(line, sizeof(line), fp))
			line[strcspn(line, "\r\n")] = '\0';
		fclose(fp);
	}
	v = strstr(line, "CFW ");
	snprintf(fw_version, sizeof(fw_version), "%s", v ? v + 4 : line);
}

void retro_deinit(void) {}

bool retro_load_game(const struct retro_game_info* game)
{
	(void)game;
	screen = SCREEN_MAIN;
	main_sel = 0;
	memset(prev_pad, 0, sizeof(prev_pad));
	sq_cart_scan(&cart);
	return true;
}

void retro_unload_game(void)
{
	/* Never leave a copy half done */
	if (worker_busy)
		pthread_join(worker, NULL);
}

void retro_run(void)
{
	unsigned pressed = ReadPresses();
	frame_count++;

	if (screen == SCREEN_BUSY) {
		if (!worker_busy) {
			pthread_join(worker, NULL);
			screen = SCREEN_MESSAGE;
		}
	} else {
		HandleInput(pressed);
		/* Notice a cartridge being inserted or removed (twice a second) */
		if (screen == SCREEN_MAIN && frame_count % 30 == 0)
			sq_cart_scan(&cart);
	}

	Render();
	video_cb(fb, WIDTH, HEIGHT, WIDTH * sizeof(uint16_t));
	audio_batch_cb(silence, 735);
}

void retro_reset(void) { screen = SCREEN_MAIN; }
void retro_set_controller_port_device(unsigned port, unsigned device) { (void)port; (void)device; }
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void* data, size_t size) { (void)data; (void)size; return false; }
bool retro_unserialize(const void* data, size_t size) { (void)data; (void)size; return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char* code) { (void)index; (void)enabled; (void)code; }
bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t num)
{
	(void)type; (void)info; (void)num;
	return false;
}
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void* retro_get_memory_data(unsigned id) { (void)id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
