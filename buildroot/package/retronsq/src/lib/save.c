/*
 * Saves: importing a cartridge's save, numbered backups, restoring a backup,
 * and checking a save for problems (see sqcart.h).
 *
 * Rules:
 *   - The cartridge is never written here (only cartwrite.c does, when the
 *     user asks for it). Its save is copied to the cache as
 *     <name>.srm the first time (sq_import_save); from then on that .srm is
 *     the live save RetroArch reads and writes.
 *   - SQ_SAVES_DIR/<name>/cartridge.sav is a copy of the cartridge's own
 *     save, refreshed each time the cartridge is played (sq_sync_cart_save).
 *   - Backups live next to it as NNN-<kind>.sav, numbered upward, made when
 *     the user asks (Back up cartridge save) or before something is
 *     overwritten (a restore, a write to the cartridge). An identical
 *     backup is never stored twice.
 *   - Every write goes through sq_write_atomic.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sqcart.h"

/* --- import ------------------------------------------------------------- */

int sq_save_size_plausible(long size)
{
	/* EEPROM 512 B / 8 KB, SRAM 32 KB, flash 64 / 128 KB, GB 2-128 KB */
	static const long sizes[] = { 512, 2048, 8192, 32768, 65536, 131072 };
	unsigned i;
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
		if (size == sizes[i])
			return 1;
	return 0;
}

int sq_import_save(const struct sq_cart* c, struct sq_error* e)
{
	char src[SQ_PATH_LEN], dst[SQ_PATH_LEN], dir[SQ_PATH_LEN];
	unsigned char* data;
	long size;
	int rc;

	sq_cart_cache_path(c, ".srm", dst, sizeof(dst));
	if (sq_file_size(dst) >= 0 || c->save_size <= 0)
		return 0;	/* the live save already exists, or nothing to import */
	sq_mkdirs(sq_path(dir, sizeof(dir), SQ_ROM_DIR));

	sq_cart_reader_path(c, ".sav", src, sizeof(src));
	data = sq_read_all(src, SQ_MAX_SAVE, &size);
	if (!data) {
		sq_fail(e, errno, "reading the cartridge's save");
		return -1;
	}
	if (!sq_save_size_plausible(size))
		sq_log("cartridge save of %s has an unusual size: %ld bytes", c->rom, size);
	if (sq_is_gen3_pokemon(c->code) && size < 131072)
		sq_log("cartridge save of %s is %ld bytes, the game uses 128 KB: "
			"the reader may have read only part of it", c->rom, size);
	rc = sq_write_atomic(dst, data, size, e);
	free(data);
	return rc;
}

/* --- backups ------------------------------------------------------------ */

static void backup_dir(const char* game, char* out, size_t len)
{
	char rel[SQ_PATH_LEN];
	snprintf(rel, sizeof(rel), "%s/%s", SQ_SAVES_DIR, game);
	sq_path(out, len, rel);
}

/* "012-before-restore.sav" -> number 12, kind "before-restore"; 0 if not a backup */
static int parse_backup_name(const char* name, char* kind, size_t len)
{
	int n = atoi(name);
	const char* dash = strchr(name, '-');
	size_t klen;

	if (n <= 0 || !dash || !sq_ends_with(name, ".sav"))
		return 0;
	klen = strlen(dash + 1) - 4;
	if (klen >= len)
		klen = len - 1;
	memcpy(kind, dash + 1, klen);
	kind[klen] = '\0';
	return n;
}

int sq_backup_store(const char* game, const void* data, long size,
	const char* kind, int* existed, struct sq_error* e)
{
	char dir[SQ_PATH_LEN], path[SQ_PATH_LEN], k[32];
	DIR* d;
	struct dirent* ent;
	int last = 0, n;

	*existed = 0;
	backup_dir(game, dir, sizeof(dir));
	sq_mkdirs(dir);

	d = opendir(dir);
	if (d) {
		while ((ent = readdir(d)) != NULL) {
			n = parse_backup_name(ent->d_name, k, sizeof(k));
			if (!n)
				continue;
			if (n > last)
				last = n;
			snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
			if (!*existed && sq_same_content(path, data, size)) {
				*existed = n;
			}
		}
		closedir(d);
	}
	if (*existed) {
		n = *existed;
		*existed = 1;
		return n;
	}
	n = last + 1;
	snprintf(path, sizeof(path), "%s/%03d-%s.sav", dir, n, kind);
	return sq_write_atomic(path, data, size, e) == 0 ? n : -1;
}

void sq_cart_save_copy_path(const char* game, char* out, size_t len)
{
	char dir[SQ_PATH_LEN];
	backup_dir(game, dir, sizeof(dir));
	snprintf(out, len, "%s/" SQ_CART_SAVE_COPY, dir);
}

/* Delete backups that hold exactly what an older backup holds (left over
 * from versions that backed up on every insert); the oldest stays */
static void dedupe_backups(const char* game)
{
	struct sq_backup list[64];
	unsigned char* data[64];
	long size[64];
	int n = sq_backup_list(game, list, 64), i, j;

	/* list is newest first: compare each with every older one */
	for (i = 0; i < n; i++)
		data[i] = sq_read_all(list[i].path, SQ_MAX_SAVE, &size[i]);
	for (i = 0; i < n; i++) {
		for (j = i + 1; j < n && data[i]; j++) {
			if (data[j] && size[i] == size[j] && !memcmp(data[i], data[j], size[i])) {
				if (unlink(list[i].path) == 0)
					sq_log_sd("saves: deleted %s, the same as backup #%d",
						list[i].path, list[j].number);
				break;
			}
		}
	}
	for (i = 0; i < n; i++)
		free(data[i]);
}

int sq_sync_cart_save(const struct sq_cart* c, int* changed, struct sq_error* e)
{
	char path[SQ_PATH_LEN], dir[SQ_PATH_LEN];
	unsigned char* data;
	long size;
	int rc = 0;

	*changed = 0;
	if (c->save_size <= 0)
		return 0;
	sq_cart_reader_path(c, ".sav", path, sizeof(path));
	data = sq_read_all(path, SQ_MAX_SAVE, &size);
	if (!data) {
		sq_fail(e, errno, "reading the cartridge's save");
		return -1;
	}
	backup_dir(c->base, dir, sizeof(dir));
	sq_mkdirs(dir);
	sq_cart_save_copy_path(c->base, path, sizeof(path));
	if (!sq_same_content(path, data, size)) {
		rc = sq_write_atomic(path, data, size, e);
		*changed = rc == 0;
	}
	free(data);
	dedupe_backups(c->base);
	return rc;
}

int sq_backup_cart_save(const struct sq_cart* c, int* existed,
	sq_progress_fn progress, void* ctx, long base, struct sq_error* e)
{
	return sq_backup_cart_save_as(c, "cartridge", existed, progress, ctx, base, e);
}

int sq_backup_cart_save_as(const struct sq_cart* c, const char* kind, int* existed,
	sq_progress_fn progress, void* ctx, long base, struct sq_error* e)
{
	char path[SQ_PATH_LEN];
	unsigned char* data;
	long size;
	int n;

	*existed = 0;
	if (c->save_size <= 0)
		return 0;
	sq_cart_reader_path(c, ".sav", path, sizeof(path));
	data = sq_read_all_progress(path, SQ_MAX_SAVE, &size, progress, ctx, base);
	if (!data) {
		sq_fail(e, errno, "reading the cartridge's save");
		return -1;
	}
	n = sq_backup_store(c->base, data, size, kind, existed, e);
	free(data);
	return n;
}

static int compare_backups(const void* a, const void* b)
{
	return ((const struct sq_backup*)b)->number - ((const struct sq_backup*)a)->number;
}

int sq_backup_list(const char* game, struct sq_backup* out, int max)
{
	char dir[SQ_PATH_LEN];
	DIR* d;
	struct dirent* ent;
	int count = 0;

	backup_dir(game, dir, sizeof(dir));
	d = opendir(dir);
	if (!d)
		return 0;
	while (count < max && (ent = readdir(d)) != NULL) {
		struct sq_backup* b = &out[count];
		b->number = parse_backup_name(ent->d_name, b->kind, sizeof(b->kind));
		if (!b->number)
			continue;
		snprintf(b->path, sizeof(b->path), "%s/%s", dir, ent->d_name);
		b->size = sq_file_size(b->path);
		count++;
	}
	closedir(d);
	qsort(out, count, sizeof(out[0]), compare_backups);
	return count;
}

int sq_restore_save(const struct sq_cart* c, const char* path,
	sq_progress_fn progress, void* ctx, struct sq_error* e)
{
	char live[SQ_PATH_LEN], dir[SQ_PATH_LEN];
	unsigned char *data, *cur;
	long size, cur_size;
	int existed, rc = -1;

	data = sq_read_all_progress(path, SQ_MAX_SAVE, &size, progress, ctx, 0);
	if (!data) {
		sq_fail(e, errno, "reading %s", path);
		return -1;
	}
	sq_mkdirs(sq_path(dir, sizeof(dir), SQ_ROM_DIR));
	sq_cart_cache_path(c, ".srm", live, sizeof(live));

	/* Never lose the save being replaced */
	cur = sq_read_all(live, SQ_MAX_SAVE, &cur_size);
	if (cur && sq_backup_store(c->base, cur, cur_size, "before-restore", &existed, e) < 0)
		sq_fail(e, e ? e->err : 0, "backing up the current save failed, nothing was changed");
	else
		rc = sq_write_atomic(live, data, size, e);
	free(cur);
	free(data);
	return rc;
}

/* --- Gen 3 Pokémon save check ------------------------------------------- */

/*
 * Ruby, Sapphire, Emerald, FireRed and LeafGreen use a 128 KB flash chip
 * holding two save slots of 14 sectors (4 KB each; sectors 0-13 and 14-27),
 * written alternately. Each sector ends with a footer:
 *   0xFF4 u16 section id (0..13)   0xFF6 u16 checksum
 *   0xFF8 u32 signature 0x08012025 0xFFC u32 save counter
 * The checksum is the 32-bit sum of the section data's words, folded to 16
 * bits. The games clear a sector before filling it, so summing the largest
 * section size (3968 bytes) gives the same result for every section. The
 * game loads the valid slot with the higher counter, and reports "the save
 * file is corrupted" when the newest slot is damaged.
 */
#define GEN3_SECTOR 0x1000
#define GEN3_DATA 3968
#define GEN3_SIGNATURE 0x08012025u

static uint32_t le32(const uint8_t* p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int sq_is_gen3_pokemon(const char* code)
{
	return code && (!strncmp(code, "AXV", 3) || !strncmp(code, "AXP", 3) ||
		!strncmp(code, "BPE", 3) || !strncmp(code, "BPR", 3) ||
		!strncmp(code, "BPG", 3));
}

void sq_gen3_check(const uint8_t* data, long size, struct sq_gen3_report* r)
{
	int s, i;

	memset(r, 0, sizeof(*r));
	r->size = size;
	r->truncated = size < 2 * 14 * GEN3_SECTOR + 4 * GEN3_SECTOR;	/* 128 KB */
	r->newest = -1;

	for (s = 0; s < 2; s++) {
		struct sq_gen3_slot* slot = &r->slot[s];
		unsigned seen = 0;
		int counter_set = 0, counters_match = 1;

		for (i = 0; i < 14; i++) {
			long off = (long)(s * 14 + i) * GEN3_SECTOR;
			const uint8_t* sec = data + off;
			uint32_t sum = 0;
			unsigned id, checksum, j;

			if (off + GEN3_SECTOR > size)
				break;		/* missing from a truncated dump */
			if (le32(sec + 0xFF8) != GEN3_SIGNATURE) {
				slot->bad_signatures++;
				continue;
			}
			id = sec[0xFF4] | sec[0xFF5] << 8;
			checksum = sec[0xFF6] | sec[0xFF7] << 8;
			if (id < 14)
				seen |= 1u << id;
			for (j = 0; j < GEN3_DATA; j += 4)
				sum += le32(sec + j);
			if ((uint16_t)((sum >> 16) + sum) != checksum)
				slot->bad_checksums++;
			if (!counter_set) {
				slot->counter = le32(sec + 0xFFC);
				counter_set = 1;
			} else if (le32(sec + 0xFFC) != slot->counter) {
				counters_match = 0;
			}
		}
		for (i = 0; i < 14; i++)
			if (seen & (1u << i))
				slot->sections++;
		slot->valid = slot->sections == 14 && !slot->bad_checksums &&
			!slot->bad_signatures && counters_match;
		if (slot->valid && (r->newest < 0 || slot->counter > r->slot[r->newest].counter))
			r->newest = s;
	}
}
