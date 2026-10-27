/*
 * The game library for the home screen (see sqcart.h): games on the SD
 * card, recently played, favorites, where their box art is, the saves,
 * and deleting games and saves.
 *
 * History and favorites are small text files in SQ_HOME_DIR, one game per
 * line: "<path><TAB><label>", paths as on the console (/userdata/...), so
 * they stay valid whatever $SQ_ROOT is. They are rewritten atomically.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sqcart.h"

#define HISTORY_FILE	SQ_HOME_DIR "/history.txt"
#define FAVORITES_FILE	SQ_HOME_DIR "/favorites.txt"
#define MAX_GAMES	4096
#define MAX_DEPTH	4

/* --- one game ----------------------------------------------------------- */

static int is_game_file(const char* name)
{
	return sq_ends_with(name, ".gba") || sq_ends_with(name, ".gbc") ||
		sq_ends_with(name, ".gb") || sq_ends_with(name, ".zip");
}

const char* sq_rom_system(const char* path)
{
	const char* p;
	if (sq_ends_with(path, ".gba"))
		return "gba";
	if (sq_ends_with(path, ".gbc"))
		return "gbc";
	if (sq_ends_with(path, ".zip")) {
		/* .../roms/gba/... : the folder says which system */
		for (p = path; (p = strstr(p, "/")) != NULL; p++) {
			if (!strncasecmp(p, "/gba/", 5))
				return "gba";
			if (!strncasecmp(p, "/gbc/", 5))
				return "gbc";
		}
	}
	return "gb";
}

const char* sq_system_name(const char* system)
{
	if (!strcmp(system, "gba"))
		return "Nintendo - Game Boy Advance";
	if (!strcmp(system, "gbc"))
		return "Nintendo - Game Boy Color";
	return "Nintendo - Game Boy";
}

/* Fill a game from its path (on the console) and an optional label; the
 * default label is the file name without extension */
static void make_game(struct sq_game* g, const char* path, const char* label)
{
	const char* base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	char* dot;
	snprintf(g->path, sizeof(g->path), "%s", path);
	snprintf(g->system, sizeof(g->system), "%s", sq_rom_system(path));
	if (label && *label) {
		snprintf(g->label, sizeof(g->label), "%s", label);
		return;
	}
	snprintf(g->label, sizeof(g->label), "%s", base);
	dot = strrchr(g->label, '.');
	if (dot && dot != g->label)
		*dot = '\0';
}

/* The console path of a sandbox path (strip $SQ_ROOT) */
static const char* unroot(const char* path)
{
	const char* root = getenv("SQ_ROOT");
	size_t n = root ? strlen(root) : 0;
	return n && !strncmp(path, root, n) ? path + n : path;
}

static int compare_games(const void* a, const void* b)
{
	return strcasecmp(((const struct sq_game*)a)->label, ((const struct sq_game*)b)->label);
}

/* --- scanning ------------------------------------------------------------ */

struct scan {
	struct sq_game* games;
	int count, cap;
	const char* system;
};

static void scan_dir(const char* dir, int depth, struct scan* s)
{
	char path[SQ_PATH_LEN];
	struct dirent* d;
	struct stat st;
	DIR* di = opendir(dir);

	if (!di)
		return;
	while ((d = readdir(di)) != NULL && s->count < MAX_GAMES) {
		if (d->d_name[0] == '.' || strchr(d->d_name, '\t') || strchr(d->d_name, '\n'))
			continue;	/* hidden (macOS ._ files too), or unlistable */
		snprintf(path, sizeof(path), "%s/%s", dir, d->d_name);
		if (stat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < MAX_DEPTH)
				scan_dir(path, depth + 1, s);
			continue;
		}
		if (!S_ISREG(st.st_mode) || !is_game_file(d->d_name))
			continue;
		if (s->system && strcmp(sq_rom_system(path), s->system) != 0)
			continue;
		if (s->count == s->cap) {
			int cap = s->cap ? s->cap * 2 : 64;
			struct sq_game* more = (struct sq_game*)realloc(s->games, cap * sizeof(*more));
			if (!more)
				break;
			s->games = more;
			s->cap = cap;
		}
		make_game(&s->games[s->count++], unroot(path), NULL);
	}
	closedir(di);
}

int sq_library_scan(const char* dir, const char* system, struct sq_game** out)
{
	char path[SQ_PATH_LEN];
	struct scan s = { NULL, 0, 0, system };

	scan_dir(sq_path(path, sizeof(path), dir), 0, &s);
	if (s.count > 1)
		qsort(s.games, s.count, sizeof(*s.games), compare_games);
	*out = s.games;
	return s.count;
}

/* --- history and favorites ----------------------------------------------- */

/* Read a list file; entries whose game is gone are left out */
static int list_load(const char* rel, struct sq_game** out)
{
	char path[SQ_PATH_LEN], line[SQ_PATH_LEN + 160], full[SQ_PATH_LEN];
	struct sq_game* games = NULL;
	int count = 0, cap = 0;
	FILE* fp = fopen(sq_path(path, sizeof(path), rel), "r");

	*out = NULL;
	if (!fp)
		return 0;
	while (fgets(line, sizeof(line), fp)) {
		char* tab = strchr(line, '\t');
		line[strcspn(line, "\r\n")] = '\0';
		if (tab)
			*tab++ = '\0';
		if (line[0] != '/' || sq_file_size(sq_path(full, sizeof(full), line)) < 0)
			continue;
		if (count == cap) {
			struct sq_game* more;
			cap = cap ? cap * 2 : 32;
			more = (struct sq_game*)realloc(games, cap * sizeof(*more));
			if (!more)
				break;
			games = more;
		}
		make_game(&games[count++], line, tab);
	}
	fclose(fp);
	*out = games;
	return count;
}

/* Write a list back (atomically) */
static int list_save(const char* rel, const struct sq_game* games, int count)
{
	char path[SQ_PATH_LEN];
	size_t cap = (size_t)count * (SQ_PATH_LEN + 140) + 1, used = 0;
	char* buf = (char*)malloc(cap);
	int i, rc;

	if (!buf)
		return -1;
	for (i = 0; i < count; i++)
		used += snprintf(buf + used, cap - used, "%s\t%s\n", games[i].path, games[i].label);
	sq_mkdirs(sq_path(path, sizeof(path), SQ_HOME_DIR));
	rc = sq_write_atomic(sq_path(path, sizeof(path), rel), buf, used, NULL);
	free(buf);
	return rc;
}

int sq_history_load(struct sq_game** out)
{
	return list_load(HISTORY_FILE, out);
}

int sq_favorites_load(struct sq_game** out)
{
	int n = list_load(FAVORITES_FILE, out);
	if (n > 1)
		qsort(*out, n, sizeof(**out), compare_games);
	return n;
}

int sq_history_add(const char* path, const char* label)
{
	struct sq_game* old = NULL;
	struct sq_game* list;
	int n = list_load(HISTORY_FILE, &old), i, count = 0, rc;

	list = (struct sq_game*)malloc((n + 1) * sizeof(*list));
	if (!list) {
		free(old);
		return -1;
	}
	make_game(&list[count++], path, label);
	for (i = 0; i < n && count < SQ_HISTORY_MAX; i++)
		if (strcmp(old[i].path, path) != 0)
			list[count++] = old[i];
	rc = list_save(HISTORY_FILE, list, count);
	free(old);
	free(list);
	return rc;
}

int sq_is_favorite(const char* path)
{
	struct sq_game* list = NULL;
	int n = list_load(FAVORITES_FILE, &list), i, found = 0;
	for (i = 0; i < n && !found; i++)
		found = !strcmp(list[i].path, path);
	free(list);
	return found;
}

int sq_favorite_toggle(const char* path, const char* label)
{
	struct sq_game* old = NULL;
	struct sq_game* list;
	int n = list_load(FAVORITES_FILE, &old), i, count = 0, was = 0;

	list = (struct sq_game*)malloc((n + 1) * sizeof(*list));
	if (!list) {
		free(old);
		return -1;
	}
	for (i = 0; i < n; i++) {
		if (!strcmp(old[i].path, path))
			was = 1;
		else
			list[count++] = old[i];
	}
	if (!was)
		make_game(&list[count++], path, label);
	i = list_save(FAVORITES_FILE, list, count);
	free(old);
	free(list);
	return i != 0 ? -1 : !was;
}

/* --- box art ------------------------------------------------------------- */

/* Next to the game first, then RetroArch's thumbnail folders (see sqcart.h) */

char* sq_boxart_path(const struct sq_game* g, char* out, size_t len)
{
	char name[sizeof(g->label)], rel[SQ_PATH_LEN];
	char *p, *dot, *slash;

	/* Simplest for users: a picture next to the game, with its name
	 * (roms/gba/Golden Sun.gba -> roms/gba/Golden Sun.png) */
	snprintf(rel, sizeof(rel), "%s", g->path);
	dot = strrchr(rel, '.');
	slash = strrchr(rel, '/');
	if (dot && (!slash || dot > slash))
		*dot = '\0';
	if (strlen(rel) + 5 < sizeof(rel)) {
		strcat(rel, ".png");
		if (sq_file_size(sq_path(out, len, rel)) > 0)
			return out;
	}

	/* RetroArch's thumbnail names replace these characters */
	snprintf(name, sizeof(name), "%s", g->label);
	for (p = name; *p; p++)
		if (strchr("&*/:`<>?\\|", *p))
			*p = '_';
	snprintf(rel, sizeof(rel), "%s/%s/Named_Boxarts/%s.png", SQ_THUMBS_DIR,
		sq_system_name(g->system), name);
	return sq_path(out, len, rel);
}

/* --- saves --------------------------------------------------------------- */

struct save_scan {
	struct sq_save_file* saves;
	int count, cap;
};

static struct sq_save_file* add_save(struct save_scan* s)
{
	if (s->count == s->cap) {
		int cap = s->cap ? s->cap * 2 : 64;
		struct sq_save_file* more = (struct sq_save_file*)realloc(s->saves, cap * sizeof(*more));
		if (!more)
			return NULL;
		s->saves = more;
		s->cap = cap;
	}
	memset(&s->saves[s->count], 0, sizeof(s->saves[0]));
	return &s->saves[s->count++];
}

/* Fill the rest of a save from its path (with $SQ_ROOT) */
static void save_stat(struct sq_save_file* f, const char* full)
{
	struct stat st;
	snprintf(f->path, sizeof(f->path), "%s", unroot(full));
	if (stat(full, &st) == 0) {
		f->size = st.st_size;
		f->mtime = st.st_mtime;
	}
}

/* "Pokemon_Emerald" -> "Pokemon Emerald" (cartridge file names) */
static void spaces(char* s)
{
	for (; *s; s++)
		if (*s == '_')
			*s = ' ';
}

/* Live saves (<game>.srm) under dir, recursively: kind is what to call them */
static void scan_srm(const char* dir, int depth, int cartridge, struct save_scan* s)
{
	char path[SQ_PATH_LEN];
	struct dirent* d;
	struct stat st;
	DIR* di = opendir(dir);

	if (!di)
		return;
	while ((d = readdir(di)) != NULL && s->count < MAX_GAMES) {
		struct sq_save_file* f;
		char* dot;
		if (d->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, d->d_name);
		if (stat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < MAX_DEPTH)
				scan_srm(path, depth + 1, cartridge, s);
			continue;
		}
		if (!S_ISREG(st.st_mode) || !sq_ends_with(d->d_name, ".srm") || !(f = add_save(s)))
			continue;
		snprintf(f->label, sizeof(f->label), "%s", d->d_name);
		dot = strrchr(f->label, '.');
		if (dot)
			*dot = '\0';
		if (cartridge) {
			spaces(f->label);
			snprintf(f->kind, sizeof(f->kind), "Save for playing the cartridge");
		} else if (strstr(path, "/gba/")) {
			snprintf(f->kind, sizeof(f->kind), "Game Boy Advance save");
		} else if (strstr(path, "/gbc/")) {
			snprintf(f->kind, sizeof(f->kind), "Game Boy Color save");
		} else if (strstr(path, "/gb/")) {
			snprintf(f->kind, sizeof(f->kind), "Game Boy save");
		} else {
			snprintf(f->kind, sizeof(f->kind), "Game save");
		}
		f->live = 1;
		save_stat(f, path);
	}
	closedir(di);
}

static int compare_saves(const void* a, const void* b)
{
	const struct sq_save_file* x = (const struct sq_save_file*)a;
	const struct sq_save_file* y = (const struct sq_save_file*)b;
	int c = strcasecmp(x->label, y->label);
	if (c)
		return c;
	if (x->live != y->live)
		return y->live - x->live;	/* the live save, */
	if (x->copy != y->copy)
		return y->copy - x->copy;	/* the cartridge's copy, */
	return y->backup - x->backup;		/* then backups, newest first */
}

int sq_saves_scan(struct sq_save_file** out)
{
	char dir[SQ_PATH_LEN], label[128], path[SQ_PATH_LEN];
	struct save_scan s = { NULL, 0, 0 };
	struct sq_backup list[64];
	struct dirent* d;
	DIR* di;
	int i, n;

	scan_srm(sq_path(dir, sizeof(dir), SQ_ROMS_DIR), 0, 0, &s);
	scan_srm(sq_path(dir, sizeof(dir), SQ_ROM_DIR), 0, 1, &s);

	/* Backups: saves/<game>/NNN-<kind>.sav */
	di = opendir(sq_path(dir, sizeof(dir), SQ_SAVES_DIR));
	while (di && (d = readdir(di)) != NULL) {
		if (d->d_name[0] == '.')
			continue;
		n = sq_backup_list(d->d_name, list, 64);
		snprintf(label, sizeof(label), "%s", d->d_name);
		spaces(label);
		/* The copy of the cartridge's own save */
		sq_cart_save_copy_path(d->d_name, path, sizeof(path));
		if (sq_file_size(path) >= 0) {
			struct sq_save_file* f = add_save(&s);
			if (f) {
				snprintf(f->label, sizeof(f->label), "%s", label);
				snprintf(f->kind, sizeof(f->kind), "Copy of the cartridge's save");
				f->copy = 1;
				save_stat(f, path);
			}
		}
		for (i = 0; i < n; i++) {
			struct sq_save_file* f = add_save(&s);
			char* p;
			if (!f)
				break;
			snprintf(f->label, sizeof(f->label), "%s", label);
			snprintf(f->kind, sizeof(f->kind), "Backup #%d (%s)", list[i].number, list[i].kind);
			for (p = f->kind; *p; p++)
				if (*p == '-')
					*p = ' ';
			f->backup = list[i].number;
			save_stat(f, list[i].path);
		}
	}
	if (di)
		closedir(di);
	if (s.count > 1)
		qsort(s.saves, s.count, sizeof(*s.saves), compare_saves);
	*out = s.saves;
	return s.count;
}

/* --- deleting ------------------------------------------------------------- */

/* path (on the console) is a file directly or deeper inside dir/ and has
 * no ".." in it */
static int inside(const char* path, const char* dir)
{
	size_t n = strlen(dir);
	return !strncmp(path, dir, n) && path[n] == '/' && path[n + 1] && !strstr(path, "/..");
}

static int remove_file(const char* path, const char* what, struct sq_error* e)
{
	char full[SQ_PATH_LEN];
	if (unlink(sq_path(full, sizeof(full), path)) != 0) {
		sq_fail(e, errno, "deleting %s", what);
		return -1;
	}
	sq_log_sd("home: deleted %s", path);
	return 0;
}

int sq_delete_game(const char* path, struct sq_error* e)
{
	char crc[SQ_PATH_LEN], full[SQ_PATH_LEN];
	char* dot;

	if (!is_game_file(path) || (!inside(path, SQ_ROMS_DIR) && !inside(path, SQ_ROM_DIR))) {
		sq_fail(e, 0, "only games in roms/ can be deleted");
		return -1;
	}
	if (remove_file(path, "the game", e) != 0)
		return -1;
	/* A cartridge copy's checksum goes with it (the copy isn't trusted
	 * any more: it is read from the cartridge again next time) */
	if (inside(path, SQ_ROM_DIR)) {
		snprintf(crc, sizeof(crc), "%s", path);
		dot = strrchr(crc, '.');
		if (dot && dot > strrchr(crc, '/'))
			strcpy(dot, ".crc");
		unlink(sq_path(full, sizeof(full), crc));
	}
	return 0;
}

int sq_delete_save(const char* path, struct sq_error* e)
{
	int live = sq_ends_with(path, ".srm") && (inside(path, SQ_ROMS_DIR) || inside(path, SQ_ROM_DIR));
	int backup = sq_ends_with(path, ".sav") && inside(path, SQ_SAVES_DIR);

	if (!live && !backup) {
		sq_fail(e, 0, "only saves in roms/ and saves/ can be deleted");
		return -1;
	}
	return remove_file(path, "the save", e);
}
