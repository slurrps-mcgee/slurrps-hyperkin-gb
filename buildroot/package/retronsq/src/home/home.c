/*
 * sqhome: the Retron SQ home screen. sq-session runs it whenever no game
 * is running (UI=home in retronsq.cfg); RetroArch only plays games.
 *
 *   +--------------------------------------------------------------+
 *   | RETRON SQ                      Cartridge: POKEMON EMER  v0.3  |
 *   | Cartridge        | Game Boy Advance . 23 games  | [box art]  |
 *   | Recently played  | > Advance Wars           *   |            |
 *   | Favorites        |   Golden Sun                 |  title     |
 *   | Game Boy Advance |   ...                        |  system    |
 *   | ...              |                              |            |
 *   | Settings         |                              |            |
 *   | A Play   Y Favorite   B Back                                  |
 *   +--------------------------------------------------------------+
 *
 * Left: pages. Right: the page. Up/Down move, A/Right go in, B/Left go
 * back, Y marks a favorite, L/R jump a screenful.
 *
 * Starting something never happens here: the choice is written to
 * $RUN/home-request (ACTION=game|cart|retroarch|reboot|poweroff, ROM=,
 * LABEL=) and sqhome exits; sq-session reads it and starts RetroArch (see
 * HandleHomeRequest in common.sh). Leaving the game brings sq-session back
 * here. Cartridge Tools' actions run here too (libsqcart's sq_do_*), in a
 * worker thread with a progress bar.
 *
 * Light on the CPU: the screen is redrawn only when something changed,
 * and the loop sleeps in SDL_WaitEventTimeout in between (waking twice a
 * second to see whether a cartridge came or went).
 *
 * Tests: SQ_ROOT (sandbox), SDL_VIDEODRIVER=dummy, SQ_HOME_SCRIPT (button
 * presses and snapshots, see run_script) and SQ_HOME_SHOTS.
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "home.h"
#include "../lib/sqcart.h"

#define REQUEST_FILE	SQ_RUN_DIR "/home-request"
#define VERSION_FILE	"/etc/retronsq-version"
#define CART_EVENT	"/usr/lib/retronsq/cart-event"
#define CART_POLL_MS	500
#define ART_DELAY_MS	150	/* load box art once the cursor rests */
#define EXIT_GRACE_S	5	/* SDL gets this long to shut down */
#define SETTLE_FRAMES	2	/* presents of the same frame after each change */
#define SETTLE_MS	33
#define WARMUP_MS	1500	/* at start: present every 100 ms this long */
#define DONE_MS		700	/* a finished copy shows "Done" this long */
#define WRITE_CHECK	SQ_RUN_DIR "/write-check.txt"	/* see PrepareCart */

/* Layout (1280x720) */
#define HEAD_H		64
#define SIDE_X		24
#define SIDE_W		296
#define TOP_Y		84
#define ROW_H		44
#define LIST_X		344
#define LIST_W		512
#define LIST_Y		140
#define LIST_ROWS	11
#define ART_X		880
#define ART_W		360
#define FOOT_Y		680

/* --- pages ------------------------------------------------------------------ */

enum page { PAGE_CART, PAGE_RECENT, PAGE_FAVORITES, PAGE_GBA, PAGE_GBC, PAGE_GB,
	PAGE_DUMPS, PAGE_SETTINGS, PAGE_COUNT };
static const char* page_name[PAGE_COUNT] = {
	"Cartridge", "Recently played", "Favorites", "Game Boy Advance",
	"Game Boy Color", "Game Boy", "Cartridge dumps", "Settings"
};

enum cart_item { CART_PLAY, CART_COPY, CART_BACKUP, CART_RESTORE, CART_WRITE, CART_COUNT };
static const char* cart_label[CART_COUNT] = {
	"Play cartridge", "Copy cartridge to SD card", "Back up cartridge save",
	"Restore a save...", "Write a save to the cartridge..."
};
static const char* cart_help[CART_COUNT] = {
	"Start the game from the cartridge.",
	"Put the game in roms/ to play it without the cartridge.",
	"Copy the cartridge's save into saves/ on the SD card.",
	"Choose which save to use with this cartridge.",
	"Experimental: put a save onto the cartridge itself."
};

enum setting { SET_AUTOLOAD, SET_WAIT, SET_GBA, SET_GBC, SET_GB, SET_UI, SET_RETROARCH,
	SET_ABOUT, SET_RESTART, SET_POWEROFF, SET_COUNT };
static const char* setting_label[SET_COUNT] = {
	"Start cartridges when inserted", "Wait for a cartridge at power-on",
	"Game Boy Advance emulator", "Game Boy Color emulator", "Game Boy emulator",
	"Start screen", "RetroArch settings", "About", "Restart", "Power off"
};
static const char* setting_key[SET_COUNT] = {
	"CART_AUTOLOAD", "CART_WAIT", "GBA_CORE", "GBC_CORE", "GB_CORE", "UI",
	NULL, NULL, NULL, NULL
};

/* --- state ------------------------------------------------------------------- */

static int page = PAGE_CART;
static int in_page;		/* focus: 0 the page list, 1 inside the page */
static int sel, top;		/* selection and scroll inside the page */
static struct sq_game* games;	/* the list pages' games */
static int game_count;
static int dirty = 1;		/* the screen needs redrawing */
static int quit;
static char version[64] = "";

static struct sq_cart cart;
static unsigned cart_generation;
static int cart_present;

/* Settings values as shown, and the emulator cores to cycle through */
static char values[SET_COUNT][32];
static char cores[16][32];
static int core_count;

/* Box art for the selected game, loaded once the cursor has rested */
static char art_path[SQ_PATH_LEN];
static Uint32 art_due;

/* Dialogs over the page */
enum dialog { DLG_NONE, DLG_CHOOSE_SAVE, DLG_CONFIRM, DLG_BUSY, DLG_RESULT, DLG_INFO };
static enum dialog dialog;
static int dlg_sel;
static struct sq_save_choice choices[65];
static int choice_count, choice_writing;
enum confirm_what { CONFIRM_RESTORE, CONFIRM_WRITE, CONFIRM_RESTART, CONFIRM_POWEROFF };
static enum confirm_what confirm_what;
static int confirm_step;
static int dlg_button;		/* confirm: 0 = No (the default), 1 = Yes */
static char info[6][64];		/* DLG_INFO lines */

/* The worker thread (cartridge actions) */
enum task { TASK_COPY, TASK_BACKUP, TASK_RESTORE, TASK_WRITE };
static pthread_t worker;
static volatile int worker_busy;
static volatile long work_done, work_total;
static struct sq_meter meter;	/* the copy's "MB of MB", speed and time left */
static char work_title[64];
static enum task task;
static char task_path[SQ_PATH_LEN];
static struct sq_result task_result;
static int task_added;

/* --- small helpers ------------------------------------------------------------ */

static int is_list_page(int p)
{
	return p >= PAGE_RECENT && p <= PAGE_DUMPS;
}

static int item_count(void)
{
	if (page == PAGE_CART)
		return cart.present ? CART_COUNT : 0;
	if (page == PAGE_SETTINGS)
		return SET_COUNT;
	return game_count;
}

static int cart_item_enabled(int i)
{
	if (!cart.present)
		return 0;
	if (i == CART_BACKUP || i == CART_WRITE)
		return cart.save_size > 0;
	return 1;
}

/* Hand something over to sq-session and leave */
static void request(const char* action, const struct sq_game* g)
{
	char path[SQ_PATH_LEN], text[SQ_PATH_LEN * 2];
	int n = snprintf(text, sizeof(text), "ACTION=%s\n", action);
	if (g) {
		n += snprintf(text + n, sizeof(text) - n, "ROM=%s\nLABEL=%s\n", g->path, g->label);
		sq_history_add(g->path, g->label);
	}
	sq_mkdirs(sq_path(path, sizeof(path), SQ_RUN_DIR));
	if (sq_write_atomic(sq_path(path, sizeof(path), REQUEST_FILE), text, n, NULL) != 0)
		sq_log("couldn't write %s", path);
	sq_log("request: %s %s", action, g ? g->path : "");
	quit = 1;
}

/* Friendly emulator names */
static const char* core_title(const char* core)
{
	if (!strcmp(core, "gpsp")) return "gpSP";
	if (!strcmp(core, "mgba")) return "mGBA";
	if (!strcmp(core, "vbam")) return "VBA-M";
	if (!strcmp(core, "gambatte")) return "Gambatte";
	if (!strcmp(core, "sameboy")) return "SameBoy";
	return core;
}

/* The cores on the console and the SD card ("gpsp", "mgba"...), sorted */
static void load_cores(void)
{
	static const char* dirs[2] = { "/usr/lib/libretro", "/userdata/retroarch/cores" };
	char path[SQ_PATH_LEN];
	struct dirent* d;
	int i, j;

	core_count = 0;
	for (i = 0; i < 2; i++) {
		DIR* dir = opendir(sq_path(path, sizeof(path), dirs[i]));
		while (dir && (d = readdir(dir)) != NULL && core_count < 16) {
			char name[32];
			char* end;
			int dup = 0;
			if (!sq_ends_with(d->d_name, "_libretro.so"))
				continue;
			snprintf(name, sizeof(name), "%s", d->d_name);
			end = strstr(name, "_libretro.so");
			if (end)
				*end = '\0';
			if (!strcmp(name, "sqtools"))
				continue;
			for (j = 0; j < core_count; j++)
				dup |= !strcmp(cores[j], name);
			if (!dup)
				snprintf(cores[core_count++], sizeof(cores[0]), "%s", name);
		}
		if (dir)
			closedir(dir);
	}
	for (i = 1; i < core_count; i++)
		for (j = i; j > 0 && strcmp(cores[j - 1], cores[j]) > 0; j--) {
			char t[32];
			memcpy(t, cores[j], sizeof(t));
			memcpy(cores[j], cores[j - 1], sizeof(t));
			memcpy(cores[j - 1], t, sizeof(t));
		}
}

static void load_settings(void)
{
	int i;
	for (i = 0; i < SET_COUNT; i++)
		if (setting_key[i])
			sq_cfg_get(setting_key[i], values[i], sizeof(values[i]));
}

/* What a setting's value looks like on screen */
static void setting_text(int i, char* out, size_t len)
{
	const char* v = values[i];
	out[0] = '\0';
	switch (i) {
	case SET_AUTOLOAD:
		snprintf(out, len, "%s", strcmp(v, "0") ? "On" : "Off");
		break;
	case SET_WAIT:
		snprintf(out, len, "%s s", v[0] ? v : "0");
		break;
	case SET_GBA:
	case SET_GBC:
	case SET_GB:
		snprintf(out, len, "%s", core_title(v));
		break;
	case SET_UI:
		snprintf(out, len, "%s", strcmp(v, "retroarch") ? "Home" : "RetroArch menu");
		break;
	default:
		break;
	}
}

/* Change a setting one step (dir -1 or +1) and save it */
static void change_setting(int i, int dir)
{
	char v[32];
	struct sq_error e;
	int j, n;

	switch (i) {
	case SET_AUTOLOAD:
		snprintf(v, sizeof(v), "%s", strcmp(values[i], "0") ? "0" : "1");
		break;
	case SET_WAIT:
		n = atoi(values[i]) + 5 * dir;
		n = n < 0 ? 0 : n > 60 ? 60 : n;
		snprintf(v, sizeof(v), "%d", n);
		break;
	case SET_GBA:
	case SET_GBC:
	case SET_GB:
		if (core_count == 0)
			return;
		for (j = 0; j < core_count && strcmp(cores[j], values[i]); j++)
			;
		j = j == core_count ? 0 : (j + dir + core_count) % core_count;
		snprintf(v, sizeof(v), "%s", cores[j]);
		break;
	case SET_UI:
		snprintf(v, sizeof(v), "%s", strcmp(values[i], "retroarch") ? "retroarch" : "home");
		break;
	default:
		return;
	}
	if (sq_cfg_set(setting_key[i], v, &e) != 0) {
		snprintf(task_result.line[0], sizeof(task_result.line[0]), "Couldn't save the setting:");
		snprintf(task_result.line[1], sizeof(task_result.line[1]), "%s", e.msg);
		task_result.line[2][0] = task_result.line[3][0] = '\0';
		task_result.ok = 0;
		dialog = DLG_RESULT;
	}
	load_settings();
}

/* --- pages' content -------------------------------------------------------------- */

static void free_games(void)
{
	free(games);
	games = NULL;
	game_count = 0;
}

static void load_page(void)
{
	int i;
	char* p;

	free_games();
	switch (page) {
	case PAGE_RECENT: game_count = sq_history_load(&games); break;
	case PAGE_FAVORITES: game_count = sq_favorites_load(&games); break;
	case PAGE_GBA: game_count = sq_library_scan(SQ_ROMS_DIR, "gba", &games); break;
	case PAGE_GBC: game_count = sq_library_scan(SQ_ROMS_DIR, "gbc", &games); break;
	case PAGE_GB: game_count = sq_library_scan(SQ_ROMS_DIR, "gb", &games); break;
	case PAGE_DUMPS:
		game_count = sq_library_scan(SQ_ROM_DIR, NULL, &games);
		/* Cartridge file names: Pokemon_Emerald -> Pokemon Emerald */
		for (i = 0; i < game_count; i++)
			for (p = games[i].label; *p; p++)
				if (*p == '_')
					*p = ' ';
		break;
	case PAGE_SETTINGS:
		load_cores();
		load_settings();
		break;
	default:
		break;
	}
	if (game_count < 0)
		game_count = 0;
	sel = top = 0;
	art_path[0] = '\0';
	art_due = SDL_GetTicks() + ART_DELAY_MS;
	ui_image(NULL, 0, 0, 0, 0);
	dirty = 1;
}

/* Keep the selection on screen */
static void scroll(void)
{
	int n = item_count();
	if (sel >= n)
		sel = n > 0 ? n - 1 : 0;
	if (sel < 0)
		sel = 0;
	if (sel < top)
		top = sel;
	if (sel >= top + LIST_ROWS)
		top = sel - LIST_ROWS + 1;
	/* New selection: its box art loads once the cursor rests */
	art_path[0] = '\0';
	art_due = SDL_GetTicks() + ART_DELAY_MS;
}

static void poll_cartridge(void)
{
	struct sq_cart c;
	sq_cart_scan(&c);
	if (c.present != cart_present || (c.present && (c.generation != cart_generation ||
			strcmp(c.rom, cart.rom)))) {
		struct sq_result r;
		cart = c;
		cart_present = c.present;
		cart_generation = c.generation;
		/* A save was written to this cartridge: now it's back in, say
		 * whether it really is on it */
		if (cart.present && sq_write_check(&cart, &r)) {
			task_result = r;
			dialog = DLG_RESULT;
		}
		if (page == PAGE_CART && in_page) {
			sel = 0;
			if (!cart.present)
				in_page = 0;
		}
		dirty = 1;
	}
}

/* --- the worker ------------------------------------------------------------------- */

static void progress(long done, void* ctx)
{
	(void)ctx;
	work_done = done;
}

static void* worker_main(void* arg)
{
	char rw[SQ_PATH_LEN + 8], path[SQ_PATH_LEN];
	(void)arg;
	switch (task) {
	case TASK_COPY:
		sq_do_copy_to_library(&cart, progress, NULL, &task_added, &task_result);
		break;
	case TASK_BACKUP:
		sq_do_backup_save(&cart, &task_result);
		break;
	case TASK_RESTORE:
		sq_do_restore_save(&cart, task_path, &task_result);
		break;
	case TASK_WRITE:
		snprintf(rw, sizeof(rw), "'%s'", sq_path(path, sizeof(path), CART_EVENT));
		sq_do_write_save(&cart, task_path, rw, &task_result);
		break;
	}
	worker_busy = 0;
	return NULL;
}

static void start_task(enum task t, const char* title, const char* path)
{
	/* Swapped or pulled since the screen last looked: say so */
	if (!sq_cart_still_there(&cart)) {
		sq_log("%s: the cartridge was removed or changed", title);
		poll_cartridge();
		memset(&task_result, 0, sizeof(task_result));
		snprintf(task_result.line[0], sizeof(task_result.line[0]),
			"The cartridge was removed or changed.");
		snprintf(task_result.line[1], sizeof(task_result.line[1]), "Check it and try again.");
		dialog = DLG_RESULT;
		return;
	}
	task = t;
	snprintf(work_title, sizeof(work_title), "%s", title);
	snprintf(task_path, sizeof(task_path), "%s", path ? path : "");
	work_done = 0;
	work_total = t == TASK_COPY ? cart.rom_size : 0;
	sq_meter_start(&meter, work_total);
	task_added = 0;
	worker_busy = 1;
	dialog = DLG_BUSY;
	if (pthread_create(&worker, NULL, worker_main, NULL) != 0) {
		worker_busy = 0;
		memset(&task_result, 0, sizeof(task_result));
		snprintf(task_result.line[0], sizeof(task_result.line[0]), "Couldn't start that.");
		dialog = DLG_RESULT;
	}
}

static void show_about(void)
{
	char path[SQ_PATH_LEN];
	struct statvfs sv;
	struct sq_game* g = NULL;
	int n = sq_library_scan(SQ_ROMS_DIR, NULL, &g);

	free(g);
	memset(info, 0, sizeof(info));
	snprintf(info[0], sizeof(info[0]), "Retron SQ CFW %s", version);
	if (statvfs(sq_path(path, sizeof(path), "/userdata"), &sv) == 0)
		snprintf(info[1], sizeof(info[1]), "SD card: %.1f GB free of %.1f GB",
			(double)sv.f_bavail * sv.f_frsize / 1e9, (double)sv.f_blocks * sv.f_frsize / 1e9);
	snprintf(info[2], sizeof(info[2]), "Games on the SD card: %d", n < 0 ? 0 : n);
	snprintf(info[3], sizeof(info[3]), "Copy ROMs into roms/gba, roms/gbc, roms/gb.");
	snprintf(info[4], sizeof(info[4]), "Box art: retroarch/thumbnails (RetroArch's).");
	dialog = DLG_INFO;
}

/* --- input ------------------------------------------------------------------------ */

static void activate(void)
{
	if (page == PAGE_CART) {
		if (!cart_item_enabled(sel))
			return;
		switch (sel) {
		case CART_PLAY: request("cart", NULL); break;
		case CART_COPY: start_task(TASK_COPY, "Copying the cartridge to the SD card...", NULL); break;
		case CART_BACKUP: start_task(TASK_BACKUP, "Backing up the save...", NULL); break;
		case CART_RESTORE:
		case CART_WRITE:
			choice_writing = sel == CART_WRITE;
			choice_count = sq_save_choices(&cart, choice_writing, choices, 65);
			dlg_sel = 0;
			dialog = DLG_CHOOSE_SAVE;
			break;
		}
		return;
	}
	if (page == PAGE_SETTINGS) {
		switch (sel) {
		case SET_RETROARCH: request("retroarch", NULL); break;
		case SET_ABOUT: show_about(); break;
		case SET_RESTART:
		case SET_POWEROFF:
			confirm_what = sel == SET_RESTART ? CONFIRM_RESTART : CONFIRM_POWEROFF;
			confirm_step = 0;
			dlg_button = 0;
			dialog = DLG_CONFIRM;
			break;
		default: change_setting(sel, 1); break;
		}
		return;
	}
	if (game_count > 0)
		request("game", &games[sel]);
}

static void handle_dialog(int b)
{
	switch (dialog) {
	case DLG_CHOOSE_SAVE:
		if (b == BTN_B || (b == BTN_A && choice_count == 0))
			dialog = DLG_NONE;
		else if (b == BTN_UP && dlg_sel > 0)
			dlg_sel--;
		else if (b == BTN_DOWN && dlg_sel < choice_count - 1)
			dlg_sel++;
		else if (b == BTN_A) {
			confirm_what = choice_writing ? CONFIRM_WRITE : CONFIRM_RESTORE;
			confirm_step = 0;
			dlg_button = 0;
			dialog = DLG_CONFIRM;
		}
		break;
	case DLG_CONFIRM:
		/* Two buttons, No and Yes: Left/Right (or Up/Down) pick, A presses
		 * the one picked, B is No */
		if (b == BTN_LEFT || b == BTN_RIGHT || b == BTN_UP || b == BTN_DOWN)
			dlg_button = !dlg_button;
		else if (b == BTN_B || (b == BTN_A && dlg_button == 0))
			dialog = confirm_what <= CONFIRM_WRITE ? DLG_CHOOSE_SAVE : DLG_NONE;
		else if (b == BTN_A && confirm_what == CONFIRM_WRITE && confirm_step == 0) {
			confirm_step = 1;	/* changing a cartridge: ask twice */
			dlg_button = 0;
		} else if (b == BTN_A) {
			switch (confirm_what) {
			case CONFIRM_RESTORE:
				start_task(TASK_RESTORE, "Restoring the save...", choices[dlg_sel].path);
				break;
			case CONFIRM_WRITE:
				start_task(TASK_WRITE, "Writing the save to the cartridge...",
					choices[dlg_sel].path);
				break;
			case CONFIRM_RESTART: request("reboot", NULL); break;
			case CONFIRM_POWEROFF: request("poweroff", NULL); break;
			}
		}
		break;
	case DLG_RESULT:
	case DLG_INFO:
		if (b == BTN_A || b == BTN_B) {
			dialog = DLG_NONE;
			if (task_added && page == PAGE_DUMPS)
				load_page();
			task_added = 0;
			poll_cartridge();
		}
		break;
	default:
		break;
	}
}

static void handle_button(int b)
{
	int n = item_count();

	dirty = 1;
	if (dialog == DLG_BUSY)
		return;
	if (dialog != DLG_NONE) {
		handle_dialog(b);
		return;
	}
	if (!in_page) {
		switch (b) {
		case BTN_UP: page = (page + PAGE_COUNT - 1) % PAGE_COUNT; load_page(); break;
		case BTN_DOWN: page = (page + 1) % PAGE_COUNT; load_page(); break;
		case BTN_A:
		case BTN_RIGHT:
			if (n > 0)
				in_page = 1;
			break;
		default:
			break;
		}
		return;
	}
	switch (b) {
	case BTN_UP: sel--; break;
	case BTN_DOWN: sel++; break;
	case BTN_L: sel -= LIST_ROWS; break;
	case BTN_R: sel += LIST_ROWS; break;
	case BTN_B: in_page = 0; break;
	case BTN_LEFT:
		if (page == PAGE_SETTINGS && setting_key[sel])
			change_setting(sel, -1);
		else
			in_page = 0;
		break;
	case BTN_RIGHT:
		if (page == PAGE_SETTINGS && setting_key[sel])
			change_setting(sel, 1);
		break;
	case BTN_A:
	case BTN_START:
		activate();
		break;
	case BTN_Y:
		if (is_list_page(page) && n > 0) {
			sq_favorite_toggle(games[sel].path, games[sel].label);
			if (page == PAGE_FAVORITES) {
				int keep = sel;
				load_page();
				sel = keep;
				in_page = game_count > 0;
			}
		}
		break;
	default:
		break;
	}
	scroll();
}

/* --- drawing ------------------------------------------------------------------------ */

/* What to call the inserted cartridge: its header title, else its file
 * name ("Pokemon_Emerald" -> "Pokemon Emerald") */
static const char* cart_name(void)
{
	static char name[sizeof(cart.base)];
	char* p;
	if (cart.title[0])
		return cart.title;
	snprintf(name, sizeof(name), "%s", cart.base);
	for (p = name; *p; p++)
		if (*p == '_')
			*p = ' ';
	return name;
}

static void draw_header(void)
{
	char text[96];
	ui_rect(0, 0, UI_W, HEAD_H, COL_SIDEBAR);
	ui_text(FONT_BOLD, 40, 16, COL_TEXT, "R E T R O N   S Q", 0);
	ui_rect(40, 48, 96, 3, COL_ACCENT);
	ui_text_right(FONT_SMALL, UI_W - 40, 20, COL_DIM, version);
	if (cart.present) {
		snprintf(text, sizeof(text), "Cartridge: %s", cart_name());
		ui_text_right(FONT_SMALL, UI_W - 60 - ui_text_width(FONT_SMALL, version), 20,
			COL_ACCENT, text);
	}
}

static void draw_sidebar(void)
{
	int i;
	ui_rect(0, HEAD_H, SIDE_X + SIDE_W + 8, UI_H - HEAD_H, COL_SIDEBAR);
	for (i = 0; i < PAGE_COUNT; i++) {
		int y = TOP_Y + i * ROW_H;
		if (i == page && !in_page) {
			ui_rect(SIDE_X, y, SIDE_W, ROW_H - 6, COL_ACCENT);
			ui_text(FONT_BOLD, SIDE_X + 16, y + 6, COL_DARK, page_name[i], SIDE_W - 24);
		} else if (i == page) {
			ui_rect(SIDE_X, y, 4, ROW_H - 6, COL_ACCENT);
			ui_text(FONT_BOLD, SIDE_X + 16, y + 6, COL_ACCENT, page_name[i], SIDE_W - 24);
		} else {
			ui_text(FONT_TEXT, SIDE_X + 16, y + 6, COL_DIM, page_name[i], SIDE_W - 24);
		}
	}
}

static void draw_row(int y, int w, const char* label, const char* value, int selected,
	int enabled, int star)
{
	unsigned c = enabled ? COL_TEXT : COL_DIM;
	if (selected && in_page) {
		ui_rect(LIST_X - 8, y, w + 16, ROW_H - 4, COL_ROW);
		ui_rect(LIST_X - 8, y, 4, ROW_H - 4, COL_ACCENT);
	}
	ui_text(FONT_TEXT, LIST_X + 8, y + 7, c, label, w - (value ? 200 : 48));
	if (value)
		ui_text_right(FONT_TEXT, LIST_X + w - 8, y + 7, selected && in_page ? COL_ACCENT : COL_DIM, value);
	if (star)
		ui_text_right(FONT_TEXT, LIST_X + w - 8, y + 7, COL_ACCENT, "\xe2\x98\x85");
}

/* The box art (or a placeholder) and details of a game */
static void draw_detail(const struct sq_game* g)
{
	char path[SQ_PATH_LEN];
	const char* sys = !strcmp(g->system, "gba") ? "GBA" : !strcmp(g->system, "gbc") ? "GBC" : "GB";

	ui_rect(ART_X, LIST_Y - 8, ART_W, ART_W, COL_PANEL);
	sq_boxart_path(g, path, sizeof(path));
	if (!art_path[0] || strcmp(art_path, path) != 0) {
		/* Not loaded yet: wait until the cursor rests (see main) */
		if ((Sint32)(SDL_GetTicks() - art_due) >= 0)
			snprintf(art_path, sizeof(art_path), "%s", path);
	}
	if (!(art_path[0] && !strcmp(art_path, path) &&
			ui_image(path, ART_X + 12, LIST_Y + 4, ART_W - 24, ART_W - 24))) {
		ui_text(FONT_TITLE, ART_X + (ART_W - ui_text_width(FONT_TITLE, sys)) / 2,
			LIST_Y - 8 + ART_W / 2 - 20, COL_DIM, sys, 0);
	}
	ui_text(FONT_BOLD, ART_X, LIST_Y + ART_W + 8, COL_TEXT, g->label, ART_W);
	ui_text(FONT_SMALL, ART_X, LIST_Y + ART_W + 42, COL_DIM, sq_system_name(g->system) + 11, ART_W);
	if (sq_is_favorite(g->path))
		ui_text(FONT_SMALL, ART_X, LIST_Y + ART_W + 70, COL_ACCENT, "\xe2\x98\x85 Favorite", 0);
}

static void draw_games(void)
{
	char head[96];
	int i;
	static const char* empty[PAGE_COUNT][2] = {
		[PAGE_RECENT] = { "Nothing played yet.", "" },
		[PAGE_FAVORITES] = { "No favorites yet.", "Press Y on a game to add it here." },
		[PAGE_GBA] = { "No Game Boy Advance games yet.", "Copy them to roms/gba on the SD card." },
		[PAGE_GBC] = { "No Game Boy Color games yet.", "Copy them to roms/gbc on the SD card." },
		[PAGE_GB] = { "No Game Boy games yet.", "Copy them to roms/gb on the SD card." },
		[PAGE_DUMPS] = { "No cartridges played yet.", "Each cartridge played is kept here." },
	};

	snprintf(head, sizeof(head), "%s", page_name[page]);
	ui_text(FONT_TITLE, LIST_X, TOP_Y, COL_TEXT, head, 0);
	if (game_count == 0) {
		ui_text(FONT_TEXT, LIST_X, LIST_Y + 8, COL_DIM, empty[page][0], 0);
		ui_text(FONT_SMALL, LIST_X, LIST_Y + 44, COL_DIM, empty[page][1], 0);
		return;
	}
	snprintf(head, sizeof(head), "%d game%s", game_count, game_count == 1 ? "" : "s");
	ui_text_right(FONT_SMALL, LIST_X + LIST_W, TOP_Y + 12, COL_DIM, head);
	for (i = 0; i < LIST_ROWS && top + i < game_count; i++) {
		const struct sq_game* g = &games[top + i];
		draw_row(LIST_Y + i * ROW_H, LIST_W, g->label, NULL, top + i == sel, 1,
			page != PAGE_FAVORITES && sq_is_favorite(g->path));
	}
	draw_detail(&games[sel]);
}

static void draw_cart_page(void)
{
	char line[96], a[16], b[16];
	int i, y;

	ui_text(FONT_TITLE, LIST_X, TOP_Y, COL_TEXT, "Cartridge", 0);
	if (!cart.present) {
		ui_text(FONT_TEXT, LIST_X, LIST_Y + 8, COL_DIM, "No cartridge inserted.", 0);
		sq_cfg_get("CART_AUTOLOAD", a, sizeof(a));
		ui_text(FONT_SMALL, LIST_X, LIST_Y + 44, COL_DIM, strcmp(a, "0")
			? "Insert one: it starts by itself." : "Insert one: it shows up here.", 0);
		return;
	}
	/* A small card with what the cartridge is */
	ui_rect(LIST_X, LIST_Y - 8, LIST_W, 96, COL_PANEL);
	ui_rect(LIST_X, LIST_Y - 8, 6, 96, COL_ACCENT);
	ui_text(FONT_BOLD, LIST_X + 20, LIST_Y, COL_TEXT, cart_name(), LIST_W - 40);
	snprintf(a, sizeof(a), "%.1f MB", cart.rom_size / (1024.0 * 1024.0));
	if (cart.save_size > 0)
		snprintf(b, sizeof(b), "%ld KB save", (cart.save_size + 1023) / 1024);
	else
		snprintf(b, sizeof(b), "no save");
	snprintf(line, sizeof(line), "%s  \xc2\xb7  %s  \xc2\xb7  %s", cart.system, a, b);
	ui_text(FONT_SMALL, LIST_X + 20, LIST_Y + 34, COL_DIM, line, LIST_W - 40);
	ui_text(FONT_SMALL, LIST_X + 20, LIST_Y + 58, COL_DIM, cart.rom, LIST_W - 40);

	y = LIST_Y + 104;
	for (i = 0; i < CART_COUNT; i++)
		draw_row(y + i * ROW_H, LIST_W, cart_label[i], NULL, i == sel, cart_item_enabled(i), 0);
	if (in_page)
		ui_text(FONT_SMALL, LIST_X, y + CART_COUNT * ROW_H + 8, COL_DIM, cart_help[sel], LIST_W);
}

static void draw_settings(void)
{
	char v[48];
	int i;
	ui_text(FONT_TITLE, LIST_X, TOP_Y, COL_TEXT, "Settings", 0);
	for (i = 0; i < SET_COUNT; i++) {
		setting_text(i, v, sizeof(v));
		draw_row(LIST_Y + i * ROW_H, LIST_W + 300, setting_label[i], v[0] ? v : NULL,
			i == sel, 1, 0);
	}
}

static void draw_footer(void)
{
	const char* hint;
	if (dialog == DLG_CHOOSE_SAVE)
		hint = "A  Choose     B  Back";
	else if (dialog == DLG_CONFIRM)
		hint = "\xe2\x97\x80 \xe2\x96\xb6  Choose     A  Press     B  No";
	else if (dialog == DLG_BUSY)
		hint = "Working...";
	else if (dialog != DLG_NONE)
		hint = "A  OK";
	else if (!in_page)
		hint = "A  Open";
	else if (page == PAGE_SETTINGS)
		hint = setting_key[sel] ? "\xe2\x97\x80 \xe2\x96\xb6  Change     B  Back" : "A  Open     B  Back";
	else if (page == PAGE_CART)
		hint = "A  Select     B  Back";
	else
		hint = "A  Play     Y  Favorite     L R  Page     B  Back";
	ui_text(FONT_SMALL, 40, FOOT_Y, COL_DIM, hint, 0);
}

/* A box in the middle with a title and lines */
/* A box in the middle: a title, lines, and buttons along the bottom (the
 * one at index sel highlighted) */
static void draw_box(const char* title, unsigned title_color, const char* const* lines, int n,
	const char* const* buttons, int nbuttons, int sel)
{
	int i, h = 120 + n * 32 + (nbuttons ? 76 : 0), y = (UI_H - h) / 2;
	ui_rect(240, y, 800, h, COL_PANEL);
	ui_rect(240, y, 800, 4, title_color);
	ui_text(FONT_BOLD, 272, y + 28, title_color, title, 736);
	for (i = 0; i < n; i++)
		ui_text(FONT_TEXT, 272, y + 76 + i * 32, COL_TEXT, lines[i], 736);
	for (i = 0; i < nbuttons; i++) {
		/* Right-aligned, in the order given */
		int bw = 200, bx = 1008 - (nbuttons - i) * (bw + 16) + 16, by = y + h - 72;
		int on = i == sel;
		ui_rect(bx, by, bw, 48, on ? COL_ACCENT : COL_ROW);
		ui_text(FONT_BOLD, bx + (bw - ui_text_width(FONT_BOLD, buttons[i])) / 2, by + 10,
			on ? COL_DARK : COL_TEXT, buttons[i], bw - 16);
	}
}

static void draw_dialog(void)
{
	static const char* const yes_no[2] = { "No", "Yes" };
	static const char* const no_write[2] = { "No", "Write it" };
	static const char* const ok[1] = { "OK" };
	const char* lines[6];
	int i, y, n;

	switch (dialog) {
	case DLG_CHOOSE_SAVE:
		ui_rect(240, 120, 800, 480, COL_PANEL);
		ui_rect(240, 120, 800, 4, COL_ACCENT);
		ui_text(FONT_BOLD, 272, 148, COL_TEXT, choice_writing
			? "Write which save to the cartridge?" : "Use which save with this cartridge?", 0);
		if (choice_count == 0) {
			ui_text(FONT_TEXT, 272, 200, COL_DIM, "No saves found for this game yet.", 0);
			break;
		}
		n = choice_count < 8 ? choice_count : 8;
		y = dlg_sel >= n ? dlg_sel - n + 1 : 0;
		for (i = 0; i < n; i++) {
			int row = 196 + i * ROW_H;
			if (y + i == dlg_sel) {
				ui_rect(256, row, 768, ROW_H - 4, COL_ROW);
				ui_rect(256, row, 4, ROW_H - 4, COL_ACCENT);
			}
			ui_text(FONT_TEXT, 276, row + 7, COL_TEXT, choices[y + i].label, 730);
		}
		break;
	case DLG_CONFIRM:
		n = 0;
		if (confirm_what == CONFIRM_RESTORE) {
			lines[n++] = choices[dlg_sel].label;
			lines[n++] = "Your current save is backed up first.";
			lines[n++] = "The cartridge itself isn't changed.";
			draw_box("Use this save when playing the cartridge?", COL_ACCENT, lines, n,
				yes_no, 2, dlg_button);
		} else if (confirm_what == CONFIRM_WRITE) {
			lines[n++] = choices[dlg_sel].label;
			lines[n++] = "The cartridge's save is backed up to the SD card first.";
			lines[n++] = "Experimental: not every cartridge slot can write.";
			lines[n++] = "Don't remove the cartridge until it's done.";
			draw_box(confirm_step ? "Sure? This changes the cartridge itself."
				: "Write this save onto the cartridge?", confirm_step ? COL_BAD : COL_ACCENT,
				lines, n, confirm_step ? no_write : yes_no, 2, dlg_button);
		} else {
			lines[n++] = "Games save as you play; nothing is lost.";
			draw_box(confirm_what == CONFIRM_RESTART ? "Restart the console?"
				: "Power off the console?", COL_ACCENT, lines, n, yes_no, 2, dlg_button);
		}
		break;
	case DLG_BUSY:
		ui_rect(240, 240, 800, 240, COL_PANEL);
		ui_rect(240, 240, 800, 4, COL_ACCENT);
		ui_text(FONT_BOLD, 272, 268, COL_TEXT, work_title, 640);
		ui_rect(272, 324, 736, 20, COL_SIDEBAR);
		if (work_total > 0) {
			/* A copy: percent, bar, "MB of MB", speed and time left, the
			 * same as the loading screen */
			char size[48], rate[64], pct[8];
			long done = !worker_busy ? work_total
				: work_done < work_total ? work_done : work_total;
			snprintf(pct, sizeof(pct), "%d%%", (int)(100.0 * done / work_total));
			ui_text_right(FONT_BOLD, 1008, 268, COL_ACCENT, pct);
			ui_rect(272, 324, (int)(736.0 * done / work_total), 20, COL_ACCENT);
			sq_meter_text(&meter, done, SDL_GetTicks(), !worker_busy, size, sizeof(size),
				rate, sizeof(rate));
			ui_text(FONT_SMALL, 272, 360, COL_DIM, size, 0);
			ui_text_right(FONT_SMALL, 1008, 360, COL_DIM, rate);
		} else {
			ui_rect(272 + (int)((SDL_GetTicks() / 8) % 656), 324, 80, 20, COL_ACCENT);
		}
		ui_text(FONT_SMALL, 272, 420, COL_DIM, "Please don't remove the cartridge.", 0);
		break;
	case DLG_RESULT:
		for (i = 1, n = 0; i < 4; i++)
			if (task_result.line[i][0])
				lines[n++] = task_result.line[i];
		draw_box(task_result.line[0], task_result.ok ? COL_OK : COL_BAD, lines, n, ok, 1, 0);
		break;
	case DLG_INFO:
		for (i = 1, n = 0; i < 6; i++)
			if (info[i][0])
				lines[n++] = info[i];
		draw_box(info[0], COL_ACCENT, lines, n, ok, 1, 0);
		break;
	default:
		break;
	}
}

static void render(void)
{
	ui_clear();
	draw_header();
	draw_sidebar();
	if (page == PAGE_CART)
		draw_cart_page();
	else if (page == PAGE_SETTINGS)
		draw_settings();
	else
		draw_games();
	draw_footer();
	if (dialog != DLG_NONE) {
		ui_shade();
		draw_dialog();
	}
	ui_present();
}

/* --- tests: scripted input ---------------------------------------------------------- */

/*
 * SQ_HOME_SCRIPT="down down a shot:menu wait sleep:200 quit": button names
 * (up down left right a b x y l r start select) are pressed one per loop,
 * "shot:NAME" saves the screen (SQ_HOME_SHOTS), "wait" waits for a
 * cartridge action to finish, "sleep:MS" waits, "quit" leaves. The script
 * running out also leaves. "joy:a1=32767,a1=0,b7=1" queues raw joystick
 * events (axis 1, button 7...) for the pretend pad of SQ_HOME_TEST_PAD=
 * "vid:pid", all at once, the way a quick tap arrives.
 */
static const char* script;
static Uint32 script_sleep_until;

static void run_script(void)
{
	static const char* names[BTN_COUNT] = { "up", "down", "left", "right", "a", "b", "x", "y",
		"l", "r", "start", "select" };
	char word[64];
	size_t n;
	int b;

	if (!script || (Sint32)(SDL_GetTicks() - script_sleep_until) < 0)
		return;
	while (*script == ' ')
		script++;
	/* "wait" holds the script until a cartridge action has finished */
	if ((worker_busy || dialog == DLG_BUSY) && !strncmp(script, "wait", 4) &&
			(script[4] == ' ' || !script[4]))
		return;
	if (!*script) {
		quit = 1;
		return;
	}
	n = strcspn(script, " ");
	snprintf(word, sizeof(word), "%.*s", (int)(n < sizeof(word) ? n : sizeof(word) - 1), script);
	script += n;

	if (!strncmp(word, "shot:", 5)) {
		render();
		ui_snapshot(word + 5);
	} else if (!strncmp(word, "sleep:", 6)) {
		script_sleep_until = SDL_GetTicks() + atoi(word + 6);
	} else if (!strcmp(word, "wait")) {
		/* worker_busy was checked before taking it */
	} else if (!strcmp(word, "quit")) {
		quit = 1;
	} else if (!strncmp(word, "joy:", 4)) {
		char* item = strtok(word + 4, ",");
		for (; item; item = strtok(NULL, ",")) {
			SDL_Event ev;
			int index = atoi(item + 1), value = atoi(strchr(item, '=') ? strchr(item, '=') + 1 : "0");
			memset(&ev, 0, sizeof(ev));
			if (item[0] == 'a') {
				ev.type = SDL_JOYAXISMOTION;
				ev.jaxis.which = -100;
				ev.jaxis.axis = index;
				ev.jaxis.value = value;
			} else if (item[0] == 'h') {
				ev.type = SDL_JOYHATMOTION;
				ev.jhat.which = -100;
				ev.jhat.hat = index;
				ev.jhat.value = value;
			} else {
				ev.type = value ? SDL_JOYBUTTONDOWN : SDL_JOYBUTTONUP;
				ev.jbutton.which = -100;
				ev.jbutton.button = index;
				ev.jbutton.state = value ? SDL_PRESSED : SDL_RELEASED;
			}
			SDL_PushEvent(&ev);
		}
	} else {
		for (b = 0; b < BTN_COUNT; b++)
			if (!strcmp(word, names[b]))
				input_inject(b);
	}
}

/* --- main ---------------------------------------------------------------------------- */

static void exit_now(int sig)
{
	(void)sig;
	_exit(0);
}

int main(void)
{
	char path[SQ_PATH_LEN];
	Uint32 last_poll = 0, last_busy_draw = 0, settle_at = 0, warm_until, last_warm = 0;
	Uint32 done_at = 0;
	int settle = 0;
	FILE* fp;

	setvbuf(stderr, NULL, _IOLBF, 0);
	unlink(sq_path(path, sizeof(path), REQUEST_FILE));
	fp = fopen(sq_path(path, sizeof(path), VERSION_FILE), "r");
	if (fp) {
		char line[128] = "", *v;
		if (fgets(line, sizeof(line), fp))
			line[strcspn(line, "\r\n")] = '\0';
		fclose(fp);
		v = strstr(line, "CFW ");
		snprintf(version, sizeof(version), "%s", v ? v + 4 : line);
	}
	if (ui_open() != 0)
		return 1;
	input_open();
	script = getenv("SQ_HOME_SCRIPT");
	if (getenv("SQ_HOME_TEST_PAD") && *getenv("SQ_HOME_TEST_PAD")) {
		const char* id = getenv("SQ_HOME_TEST_PAD");
		input_test_pad(atoi(id), strchr(id, ':') ? atoi(strchr(id, ':') + 1) : 0);
	}

	sq_cart_scan(&cart);
	cart_present = cart.present;
	cart_generation = cart.generation;
	if (dialog == DLG_NONE && cart.present && sq_write_check(&cart, &task_result))
		dialog = DLG_RESULT;
	/* The session checked a save written to the cartridge (PrepareCart,
	 * when it played it): show the verdict */
	fp = fopen(sq_path(path, sizeof(path), WRITE_CHECK), "r");
	if (fp) {
		char line[128];
		int i = 0;
		memset(&task_result, 0, sizeof(task_result));
		while (fgets(line, sizeof(line), fp)) {
			line[strcspn(line, "\r\n")] = '\0';
			if (!strncmp(line, "OK=", 3))
				task_result.ok = line[3] == '1';
			else if (i < 4)
				snprintf(task_result.line[i++], sizeof(task_result.line[0]), "%s", line);
		}
		fclose(fp);
		unlink(path);
		if (i > 0)
			dialog = DLG_RESULT;
	}

	/* Open on the cartridge when there is one, else on the games */
	page = cart.present ? PAGE_CART : PAGE_RECENT;
	load_page();
	if (page == PAGE_RECENT && game_count == 0) {
		page = PAGE_GBA;
		load_page();
	}

	/* The first moments on screen: the display may still be switching
	 * mode (after RetroArch, or at boot), and frames presented before it
	 * settles can be lost. Keep presenting for a little while. */
	warm_until = SDL_GetTicks() + WARMUP_MS;

	while (!quit) {
		SDL_Event ev;
		Uint32 now = SDL_GetTicks();
		int wait = input_repeat(now), b;

		/* Sleep until something happens: input, a cartridge poll, the box
		 * art delay, the progress bar, or the test script */
		if (wait < 0 || wait > CART_POLL_MS)
			wait = CART_POLL_MS;
		if (dialog == DLG_BUSY && wait > 100)
			wait = 100;
		if (!art_path[0] && (Sint32)(art_due - now) > 0 && (int)(art_due - now) < wait)
			wait = art_due - now + 1;
		if (script && wait > 20)
			wait = 20;
		if ((Sint32)(warm_until - now) > 0 && wait > 100)
			wait = 100;
		if (settle && (int)(settle_at - now) < wait)
			wait = (Sint32)(settle_at - now) > 0 ? (int)(settle_at - now) : 0;
		if (dirty)
			wait = 0;
		if (SDL_WaitEventTimeout(&ev, wait)) {
			do {
				if (ev.type == SDL_QUIT)
					quit = 1;	/* SIGTERM from sq-session (KillGame) */
				else if (ev.type == SDL_WINDOWEVENT)
					dirty = 1;
				input_event(&ev);
			} while (SDL_PollEvent(&ev));
		}
		now = SDL_GetTicks();
		input_repeat(now);
		while ((b = input_next()) >= 0 && !quit)
			handle_button(b);

		if (now - last_poll >= CART_POLL_MS) {
			last_poll = now;
			if (dialog == DLG_NONE)
				poll_cartridge();
		}
		if (is_list_page(page) && game_count > 0 && !art_path[0] &&
				(Sint32)(now - art_due) >= 0)
			dirty = 1;
		if (dialog == DLG_BUSY) {
			/* A finished copy stays a moment at 100% "Done", so the end of
			 * the bar is seen, then the result */
			if (!worker_busy && work_total > 0 && !done_at) {
				done_at = now;
				dirty = 1;
			}
			if (!worker_busy && (work_total == 0 || now - done_at >= DONE_MS)) {
				done_at = 0;
				pthread_join(worker, NULL);
				sq_log("%s %s %s", work_title, task_result.line[0], task_result.line[1]);
				dialog = DLG_RESULT;
				dirty = 1;
			} else if (now - last_busy_draw >= 100) {
				last_busy_draw = now;
				dirty = 1;
			}
		}
		run_script();
		if (dirty && !quit) {
			render();
			dirty = 0;
			/* Present the same frame a couple more times: on the console's
			 * display (KMSDRM, Mali) a frame only shows once more follow
			 * it, and this screen otherwise draws once and sleeps (it
			 * stayed black until a button was pressed) */
			settle = SETTLE_FRAMES;
			settle_at = now + SETTLE_MS;
		} else if (settle && !quit && (Sint32)(now - settle_at) >= 0) {
			render();
			settle--;
			settle_at = now + SETTLE_MS;
		} else if (!quit && (Sint32)(warm_until - now) > 0 && now - last_warm >= 100) {
			render();
			last_warm = now;
		}
	}

	/* Never leave a cartridge action half done */
	if (worker_busy)
		pthread_join(worker, NULL);
	free_games();
	/* Shutting SDL down hands the screen back; it mustn't keep the game
	 * waiting (the request is already written) */
	signal(SIGALRM, exit_now);
	alarm(EXIT_GRACE_S);
	input_close();
	ui_close();
	return 0;
}
