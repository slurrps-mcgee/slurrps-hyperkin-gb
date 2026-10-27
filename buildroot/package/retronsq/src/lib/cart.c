/*
 * The cartridge reader and the cartridge in it: reader state, scanning the
 * cartridge, the cache of copied cartridges, and copying a cartridge into
 * the user's library (see sqcart.h).
 *
 * After a hot-swap the old cartridge's mount can go stale: its directory
 * still lists the old files from cache, but reads fail. So a cartridge only
 * counts as present when the mount's device still exists with a medium
 * (sq_reader_state), and every copy checks the cartridge didn't change
 * underneath it (sq_cart_still_there, via cart-event's generation counter).
 */
#include <dirent.h>
#include <errno.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "sqcart.h"

/* --- reader state ------------------------------------------------------- */

/* Size in 512-byte sectors of block device dev (e.g. "sda"), -1 if gone.
 * Partitions live under their disk in sysfs, /sys/class/block covers both. */
static long block_size(const char* dev)
{
	char path[SQ_PATH_LEN], rel[128];
	FILE* fp;
	long size = -1;

	snprintf(rel, sizeof(rel), "/sys/class/block/%s/size", dev);
	fp = fopen(sq_path(path, sizeof(path), rel), "r");
	if (!fp)
		return -1;
	if (fscanf(fp, "%ld", &size) != 1)
		size = -1;
	fclose(fp);
	return size;
}

/* The device mounted at SQ_READER_DIR ("sda1"), or 0 when nothing is */
static int reader_mount(char* dev, size_t len)
{
	char path[SQ_PATH_LEN], mnt[SQ_PATH_LEN], line[SQ_PATH_LEN + 300], src[256], dir[SQ_PATH_LEN];
	FILE* fp = fopen(sq_path(path, sizeof(path), "/proc/mounts"), "r");
	int found = 0;

	if (!fp)
		return 0;
	sq_path(mnt, sizeof(mnt), SQ_READER_DIR);
	/* The last entry wins: a stale mount may still be listed under a new one */
	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "%255s %1023s", src, dir) != 2)
			continue;
		if (strcmp(dir, mnt) == 0) {
			const char* slash = strrchr(src, '/');
			snprintf(dev, len, "%s", slash ? slash + 1 : src);
			found = 1;
		}
	}
	fclose(fp);
	return found;
}

enum sq_reader_state sq_reader_state(void)
{
	char path[SQ_PATH_LEN], pattern[SQ_PATH_LEN], dev[64];
	glob_t g;
	size_t i;
	enum sq_reader_state state = SQ_READER_PENDING;

	/* Mounted and its device still has a medium: a cartridge is ready */
	if (reader_mount(dev, sizeof(dev)) && block_size(dev) > 0)
		return SQ_READER_READY;

	if (access(sq_path(path, sizeof(path), SQ_READER_PORT), F_OK) != 0)
		return SQ_READER_NONE;

	/* The reader's disks live under <port>/3-1:1.1, in host N, target N, LUN, block (see pattern) */
	snprintf(pattern, sizeof(pattern), "%s/3-1:1.1/host*/target*/*/block/sd*", path);
	if (glob(pattern, 0, NULL, &g) != 0)
		return SQ_READER_PENDING;	/* reader there, no disk yet */
	for (i = 0; i < g.gl_pathc; i++) {
		const char* name = strrchr(g.gl_pathv[i], '/') + 1;
		/* A disk with no medium reports size 0: no cartridge */
		state = block_size(name) > 0 ? SQ_READER_PENDING : SQ_READER_EMPTY;
	}
	globfree(&g);
	return state;
}

const char* sq_reader_state_name(enum sq_reader_state s)
{
	switch (s) {
	case SQ_READER_NONE:    return "none";
	case SQ_READER_PENDING: return "pending";
	case SQ_READER_EMPTY:   return "empty";
	case SQ_READER_READY:   return "ready";
	}
	return "?";
}

unsigned sq_reader_generation(void)
{
	char path[SQ_PATH_LEN];
	FILE* fp = fopen(sq_path(path, sizeof(path), SQ_RUN_DIR "/cart-generation"), "r");
	unsigned gen = 0;

	if (fp) {
		if (fscanf(fp, "%u", &gen) != 1)
			gen = 0;
		fclose(fp);
	}
	return gen;
}

/* --- the cartridge ------------------------------------------------------ */

static int is_rom(const char* name)
{
	return name[0] != '.' && (sq_ends_with(name, ".gb") ||
		sq_ends_with(name, ".gbc") || sq_ends_with(name, ".gba"));
}

/* Copy n header bytes as printable text, trimming padding */
static void header_text(char* out, const unsigned char* in, int n)
{
	int i;
	/* Titles are upper-case letters, digits and a little punctuation. Stop
	 * at anything else (GBC titles end in a flag byte), and drop a "title"
	 * of fewer than 3 characters: that is a garbled or blank header, not a
	 * name to show. */
	for (i = 0; i < n && in[i] && (strchr(" .,!&'_:+-", in[i]) ||
			(in[i] >= '0' && in[i] <= '9') || (in[i] >= 'A' && in[i] <= 'Z') ||
			(in[i] >= 'a' && in[i] <= 'z')); i++)
		out[i] = (char)in[i];
	out[i] = '\0';
	while (i > 0 && out[i - 1] == ' ')
		out[--i] = '\0';
	if (i < 3)
		out[0] = '\0';
}

void sq_rom_header(const char* path, char title[17], char code[5])
{
	unsigned char h[0x150];
	FILE* fp = fopen(path, "rb");
	int gba = sq_ends_with(path, ".gba");

	title[0] = code[0] = '\0';
	if (!fp)
		return;
	if (fread(h, 1, sizeof(h), fp) == sizeof(h)) {
		if (gba) {
			header_text(title, h + 0xA0, 12);
			header_text(code, h + 0xAC, 4);
		} else {
			header_text(title, h + 0x134, 16);
		}
	}
	fclose(fp);
}

void sq_cart_reader_path(const struct sq_cart* c, const char* ext, char* out, size_t len)
{
	char dir[SQ_PATH_LEN];
	sq_path(dir, sizeof(dir), SQ_READER_DIR);
	if (ext)
		snprintf(out, len, "%s/%s%s", dir, c->base, ext);
	else
		snprintf(out, len, "%s/%s", dir, c->rom);
}

void sq_cart_cache_path(const struct sq_cart* c, const char* ext, char* out, size_t len)
{
	char dir[SQ_PATH_LEN];
	sq_path(dir, sizeof(dir), SQ_ROM_DIR);
	if (ext)
		snprintf(out, len, "%s/%s%s", dir, c->base, ext);
	else
		snprintf(out, len, "%s/%s", dir, c->rom);
}

void sq_cart_scan(struct sq_cart* c)
{
	char dir[SQ_PATH_LEN], path[SQ_PATH_LEN];
	DIR* d;
	struct dirent* ent;
	char* dot;

	memset(c, 0, sizeof(*c));
	c->generation = sq_reader_generation();
	if (sq_reader_state() != SQ_READER_READY)
		return;
	d = opendir(sq_path(dir, sizeof(dir), SQ_READER_DIR));
	if (!d)
		return;
	/* The reader shows one ROM; pick the first by name to be predictable */
	while ((ent = readdir(d)) != NULL)
		if (is_rom(ent->d_name) && (!c->rom[0] || strcmp(ent->d_name, c->rom) < 0))
			snprintf(c->rom, sizeof(c->rom), "%s", ent->d_name);
	closedir(d);
	if (!c->rom[0])
		return;

	c->present = 1;
	snprintf(c->base, sizeof(c->base), "%s", c->rom);
	dot = strrchr(c->base, '.');
	if (dot)
		*dot = '\0';
	if (sq_ends_with(c->rom, ".gba")) {
		c->system = "Game Boy Advance";
		c->sysdir = "gba";
	} else if (sq_ends_with(c->rom, ".gbc")) {
		c->system = "Game Boy Color";
		c->sysdir = "gbc";
	} else {
		c->system = "Game Boy";
		c->sysdir = "gb";
	}
	sq_cart_reader_path(c, NULL, path, sizeof(path));
	c->rom_size = sq_file_size(path);
	sq_rom_header(path, c->title, c->code);
	sq_cart_reader_path(c, ".sav", path, sizeof(path));
	c->save_size = sq_file_size(path);
	if (c->save_size < 0)
		c->save_size = 0;
}

int sq_cart_still_there(const struct sq_cart* c)
{
	char path[SQ_PATH_LEN];
	if (!c->present || sq_reader_state() != SQ_READER_READY ||
			sq_reader_generation() != c->generation)
		return 0;
	sq_cart_reader_path(c, NULL, path, sizeof(path));
	return sq_file_size(path) == c->rom_size;
}

/* --- the cache (SQ_ROM_DIR) --------------------------------------------- */

int sq_cache_valid(const struct sq_cart* c)
{
	char path[SQ_PATH_LEN];
	uint32_t reader_crc, cached_crc;

	if (!c->present)
		return 0;
	sq_cart_cache_path(c, NULL, path, sizeof(path));
	if (sq_file_size(path) != c->rom_size)
		return 0;
	/* Only verified copies get a .crc next to them (sq_cache_cartridge) */
	sq_cart_cache_path(c, ".crc", path, sizeof(path));
	if (sq_crc_read_file(path, &cached_crc) != 0)
		return 0;
	sq_cart_reader_path(c, ".crc", path, sizeof(path));
	if (sq_crc_read_file(path, &reader_crc) != 0)
		return 0;
	return reader_crc == cached_crc;
}

/*
 * Free space for a new cartridge copy by deleting old copies in the cache,
 * largest first. Only ROMs (and leftover .part files) go: they can be read
 * from their cartridge again. Saves and save states are never deleted.
 */
static void make_room(long needed)
{
	char dir[SQ_PATH_LEN], path[SQ_PATH_LEN];

	sq_path(dir, sizeof(dir), SQ_ROM_DIR);
	for (;;) {
		char victim[SQ_PATH_LEN] = "";
		long victim_size = -1;
		unsigned long long avail;
		struct statvfs sv;
		struct dirent* ent;
		DIR* d;

		if (statvfs(dir, &sv) != 0)
			return;
		avail = (unsigned long long)sv.f_bavail * sv.f_bsize;
		if (avail >= (unsigned long long)needed)
			return;

		d = opendir(dir);
		if (!d)
			return;
		while ((ent = readdir(d)) != NULL) {
			long size;
			if (!is_rom(ent->d_name) && !sq_ends_with(ent->d_name, ".part"))
				continue;
			snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
			size = sq_file_size(path);
			if (size > victim_size) {
				victim_size = size;
				snprintf(victim, sizeof(victim), "%s", path);
			}
		}
		closedir(d);
		if (!victim[0])
			return;		/* nothing left that may be deleted */
		sq_log("low on space, deleting old cartridge copy %s", victim);
		unlink(victim);
	}
}

int sq_cache_cartridge(const struct sq_cart* c, int* verified,
	sq_progress_fn progress, void* ctx, struct sq_error* e)
{
	char src[SQ_PATH_LEN], dst[SQ_PATH_LEN], crc_path[SQ_PATH_LEN], dir[SQ_PATH_LEN];
	uint32_t expected = 0, crc = 0;
	int have_crc, attempt, copied = 0;

	*verified = 0;
	if (!sq_cart_still_there(c)) {
		sq_fail(e, 0, "the cartridge was removed or changed");
		return -1;
	}
	sq_mkdirs(sq_path(dir, sizeof(dir), SQ_ROM_DIR));
	make_room(c->rom_size + c->save_size + 4096);

	sq_cart_reader_path(c, NULL, src, sizeof(src));
	sq_cart_cache_path(c, NULL, dst, sizeof(dst));
	sq_cart_reader_path(c, ".crc", crc_path, sizeof(crc_path));
	have_crc = sq_crc_read_file(crc_path, &expected) == 0;

	for (attempt = 0; attempt < 2 && !*verified; attempt++) {
		if (sq_copy(src, dst, &crc, progress, ctx, e) != 0) {
			/* Say so plainly when the cartridge went away mid-copy */
			if (!sq_cart_still_there(c))
				sq_fail(e, 0, "the cartridge was removed while it was being read");
			continue;
		}
		copied = 1;
		if (!have_crc) {
			sq_log("no .crc from the reader, can't verify %s", c->rom);
			break;
		}
		*verified = crc == expected;
		if (!*verified)
			sq_log("crc mismatch for %s: read %08x, reader says %08x",
				c->rom, crc, expected);
	}
	if (!copied)
		return -1;
	/* Without a matching CRC only the cartridge still being there vouches
	 * for the data: a copy that "worked" while it was pulled is dropped */
	if (!*verified && !sq_cart_still_there(c)) {
		unlink(dst);
		sq_fail(e, 0, "the cartridge was removed while it was being read");
		return -1;
	}

	/* A cached .crc means "verified copy" to sq_cache_valid */
	sq_cart_cache_path(c, ".crc", dst, sizeof(dst));
	if (*verified) {
		uint8_t b[4] = { expected >> 24, expected >> 16, expected >> 8, expected };
		sq_write_atomic(dst, b, 4, NULL);
	} else {
		unlink(dst);
	}
	return 0;
}

/* --- copying into the library (SQ_ROMS_DIR) ----------------------------- */

int sq_dump_to_library(const struct sq_cart* c, char* out, size_t len,
	int* already, int* verified, int* save_copied,
	sq_progress_fn progress, void* ctx, struct sq_error* e)
{
	char src[SQ_PATH_LEN], dir[SQ_PATH_LEN], rel[SQ_PATH_LEN], path[SQ_PATH_LEN];
	char base[300], save_src[SQ_PATH_LEN], save_dst[SQ_PATH_LEN];
	const char* ext = strrchr(c->rom, '.');
	uint32_t expected = 0, crc = 0;
	int have_crc, attempt, n;

	*already = *verified = *save_copied = 0;
	if (!sq_cart_still_there(c)) {
		sq_fail(e, 0, "the cartridge was removed or changed");
		return -1;
	}
	sq_cart_reader_path(c, ".crc", path, sizeof(path));
	have_crc = sq_crc_read_file(path, &expected) == 0;

	/* A verified copy from an earlier play holds the same data, and reading
	 * it from the SD card is much faster than from the cartridge */
	if (sq_cache_valid(c))
		sq_cart_cache_path(c, NULL, src, sizeof(src));
	else
		sq_cart_reader_path(c, NULL, src, sizeof(src));

	snprintf(rel, sizeof(rel), "%s/%s", SQ_ROMS_DIR, c->sysdir);
	sq_mkdirs(sq_path(dir, sizeof(dir), rel));
	snprintf(base, sizeof(base), "%s", c->base);
	snprintf(out, len, "%s/%s", dir, c->rom);

	/* Already in the library? Don't copy it twice. A different game with
	 * the same name becomes "<name> (2)" and so on. */
	for (n = 2; sq_file_size(out) >= 0; n++) {
		if (have_crc && sq_file_size(out) == c->rom_size &&
				sq_file_crc(out, &crc, progress, ctx, NULL) == 0 && crc == expected) {
			*already = *verified = 1;
			return 0;
		}
		snprintf(base, sizeof(base), "%s (%d)", c->base, n);
		snprintf(out, len, "%s/%s%s", dir, base, ext ? ext : "");
	}

	for (attempt = 0; attempt < 2 && !*verified; attempt++) {
		if (sq_copy(src, out, &crc, progress, ctx, e) != 0) {
			if (!sq_cart_still_there(c))
				sq_fail(e, 0, "the cartridge was removed while it was being read");
			/* "out" is a new name (see above): drop an earlier attempt */
			unlink(out);
			return -1;
		}
		*verified = !have_crc || crc == expected;
	}
	/* As in sq_cache_cartridge: unverified data from a cartridge that is
	 * gone now isn't kept */
	if ((!have_crc || !*verified) && !sq_cart_still_there(c)) {
		unlink(out);
		sq_fail(e, 0, "the cartridge was removed while it was being read");
		return -1;
	}

	/* Bring the save along: the live save from playing the cartridge, else
	 * the cartridge's own. An existing save in the library is kept. */
	snprintf(save_dst, sizeof(save_dst), "%s/%s.srm", dir, base);
	sq_cart_cache_path(c, ".srm", save_src, sizeof(save_src));
	if (sq_file_size(save_src) < 0)
		sq_cart_reader_path(c, ".sav", save_src, sizeof(save_src));
	if (sq_file_size(save_dst) < 0 && sq_file_size(save_src) >= 0) {
		long size;
		unsigned char* data = sq_read_all(save_src, SQ_MAX_SAVE, &size);
		*save_copied = data && sq_write_atomic(save_dst, data, size, NULL) == 0;
		free(data);
	}
	return 0;
}
