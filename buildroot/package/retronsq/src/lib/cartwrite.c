/*
 * Writing to the cartridge: the one thing in the firmware that ever
 * changes a cartridge, for "Write a save to the cartridge" and "Write a
 * game to the cartridge", which the user asks for and confirms (see
 * sqcart.h for the API, actions.c for the steps around it).
 *
 * The reader shows the cartridge as a small FAT drive. Whether it passes
 * writes to the save file on to the save chip depends on the reader's own
 * firmware, so:
 *   - the drive's write protection, as reported when it was mounted, is
 *     respected (sq_reader_writable);
 *   - the save (or game) file is overwritten in place, never grown:
 *     nothing on the drive but that file's data changes;
 *   - the result is read back past the page cache, so what is compared is
 *     what the reader returns, not what Linux remembers writing.
 */
#define _GNU_SOURCE	/* O_DIRECT */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sqcart.h"

/* What the last write put on which cartridge, until it is checked */
#define CHECK_FILE SQ_HOME_DIR "/cart-write-check"

/* cart-event writes "0" or "1" here: the drive's read-only flag before it
 * locked it (the kernel sets it when the device reports write protection) */
#define WP_FILE SQ_RUN_DIR "/reader-wp"

int sq_reader_writable(void)
{
	char path[SQ_PATH_LEN];
	int c;
	FILE* fp = fopen(sq_path(path, sizeof(path), WP_FILE), "r");

	if (!fp)
		return -1;
	c = fgetc(fp);
	fclose(fp);
	return c == '0' ? 1 : c == '1' ? 0 : -1;
}

#define CHUNK (64 * 1024)	/* writes and read-backs go in these steps */

/* Open path for reading past the page cache: O_DIRECT, or where that isn't
 * supported, a plain open with the file's cached pages dropped */
static int open_uncached(const char* path, int* direct)
{
	int fd = open(path, O_RDONLY | O_DIRECT);
	*direct = fd >= 0;
	if (fd < 0 && errno == EINVAL) {
		fd = open(path, O_RDONLY);
		if (fd >= 0)
			posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	}
	return fd;
}

/*
 * Read size bytes of path back past the page cache, so what is compared is
 * what the reader returns, not what Linux remembers writing, and compare
 * them with data (in memory) or, when data is NULL, with the file src_fd.
 * Progress: base + bytes compared. Returns 0 when identical.
 */
static int read_back(const char* path, long size, const unsigned char* data, int src_fd,
	sq_progress_fn progress, void* ctx, long base, const char* what, struct sq_error* e)
{
	unsigned char *buf = NULL, *want = NULL;
	long off = 0;
	int direct, fd = open_uncached(path, &direct), rc = -1;

	if (fd < 0) {
		sq_fail(e, errno, "reading %s back", what);
		return -1;
	}
	if (posix_memalign((void**)&buf, 4096, CHUNK) != 0 ||
			(!data && !(want = (unsigned char*)malloc(CHUNK)))) {
		sq_fail(e, ENOMEM, "reading %s back", what);
		goto out;
	}
	while (off < size) {
		long len = size - off < CHUNK ? size - off : CHUNK;
		/* O_DIRECT reads whole 512-byte blocks */
		long ask = direct ? (len + 511) / 512 * 512 : len;
		ssize_t n = pread(fd, buf, ask, off);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EINVAL && direct && off == 0) {
			/* O_DIRECT refused at read time: plain reads, cache dropped */
			close(fd);
			fd = open(path, O_RDONLY);
			if (fd < 0)
				break;
			posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
			direct = 0;
			continue;
		}
		if (n < len) {
			sq_fail(e, n < 0 ? errno : EIO, "reading %s back", what);
			goto out;
		}
		if (!data && pread(src_fd, want, len, off) != len) {
			sq_fail(e, errno, "reading the game to compare");
			goto out;
		}
		if (memcmp(buf, data ? data + off : want, len) != 0) {
			sq_fail(e, 0, "the cartridge reads back different data%s",
				off ? " (part of it was written)" : "");
			sq_log_sd("tools: %s reads back different from %ld bytes on", what, off);
			goto out;
		}
		off += len;
		if (progress)
			progress(base + off, ctx);
	}
	rc = off < size ? -1 : 0;
	if (rc)
		sq_fail(e, errno, "reading %s back", what);
out:
	if (fd >= 0)
		close(fd);
	free(buf);
	free(want);
	return rc;
}

/* Write size bytes over the start of the file at path, in place: no
 * O_TRUNC, no O_CREAT, no temporary file and rename, so nothing on the
 * drive but that file's data changes. From data, or src_fd when NULL. */
static int write_in_place(const char* path, long size, const unsigned char* data, int src_fd,
	sq_progress_fn progress, void* ctx, long base, const char* what, struct sq_error* e)
{
	unsigned char* chunk = data ? NULL : (unsigned char*)malloc(CHUNK);
	long done = 0;
	int fd = open(path, O_WRONLY);

	if (fd < 0 || (!data && !chunk)) {
		sq_fail(e, fd < 0 ? errno : ENOMEM, "opening %s for writing", what);
		if (fd >= 0)
			close(fd);
		free(chunk);
		return -1;
	}
	while (done < size) {
		long len = size - done < CHUNK ? size - done : CHUNK;
		const unsigned char* p = data ? data + done : chunk;
		long put = 0;
		if (!data && pread(src_fd, chunk, len, done) != len) {
			sq_fail(e, errno, "reading the game to write");
			goto fail;
		}
		while (put < len) {
			ssize_t n = pwrite(fd, p + put, len - put, done + put);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				sq_fail(e, n < 0 ? errno : EIO, "writing %s", what);
				goto fail;
			}
			put += n;
		}
		done += len;
		if (progress)
			progress(base + done, ctx);
	}
	if (fsync(fd) != 0) {
		sq_fail(e, errno, "writing %s", what);
		goto fail;
	}
	close(fd);
	free(chunk);
	return 0;
fail:
	close(fd);
	free(chunk);
	return -1;
}

int sq_write_cart_save(const struct sq_cart* c, const void* data, long size,
	sq_progress_fn progress, void* ctx, long base, struct sq_error* e)
{
	char path[SQ_PATH_LEN];

	if (!sq_cart_still_there(c)) {
		sq_fail(e, 0, "the cartridge was removed or changed");
		return -1;
	}
	if (c->save_size <= 0) {
		sq_fail(e, 0, "this cartridge has no save file");
		return -1;
	}
	if (size != c->save_size) {
		sq_fail(e, 0, "the save is %ld bytes, the cartridge's is %ld", size, c->save_size);
		return -1;
	}
	sq_cart_reader_path(c, ".sav", path, sizeof(path));
	if (write_in_place(path, size, (const unsigned char*)data, -1, progress, ctx, base,
			"the cartridge's save", e) != 0)
		return -1;
	return read_back(path, size, (const unsigned char*)data, -1, progress, ctx, base + size,
		"the cartridge's save", e);
}

/* --- writing a game ------------------------------------------------------- */

int sq_cart_rom_fits(const struct sq_cart* c, const char* path, struct sq_error* e)
{
	long size = sq_file_size(path);
	int gba = sq_ends_with(path, ".gba");

	if (!c->present) {
		sq_fail(e, 0, "there is no cartridge");
		return -1;
	}
	if (!gba && !sq_ends_with(path, ".gb") && !sq_ends_with(path, ".gbc")) {
		sq_fail(e, 0, "only .gba, .gbc and .gb files can be written (unzip it first)");
		return -1;
	}
	if (gba != !strcmp(c->sysdir, "gba")) {
		sq_fail(e, 0, "a %s game can't go on a %s cartridge",
			gba ? "Game Boy Advance" : "Game Boy", c->system);
		return -1;
	}
	if (size <= 0) {
		sq_fail(e, errno, "reading %s", path);
		return -1;
	}
	if (size > c->rom_size) {
		sq_fail(e, 0, "the game is %.1f MB, the cartridge holds %.1f MB",
			size / 1048576.0, c->rom_size / 1048576.0);
		return -1;
	}
	return 0;
}

int sq_write_cart_rom(const struct sq_cart* c, const char* path,
	sq_progress_fn progress, void* ctx, long base, struct sq_error* e)
{
	char dst[SQ_PATH_LEN];
	long size = sq_file_size(path);
	int src, rc;

	if (sq_cart_rom_fits(c, path, e) != 0)
		return -1;
	if (!sq_cart_still_there(c)) {
		sq_fail(e, 0, "the cartridge was removed or changed");
		return -1;
	}
	src = open(path, O_RDONLY);
	if (src < 0) {
		sq_fail(e, errno, "opening %s", path);
		return -1;
	}
	sq_cart_reader_path(c, NULL, dst, sizeof(dst));
	rc = write_in_place(dst, size, NULL, src, progress, ctx, base, "the cartridge's game", e);
	if (rc == 0)
		rc = read_back(dst, size, NULL, src, progress, ctx, base + size,
			"the cartridge's game", e);
	close(src);
	return rc;
}

/* --- did the write stick? -------------------------------------------------- */

/* How much of a written game the check compares (read from the slot when
 * a cartridge is inserted, so kept small) */
#define ROM_CHECK_SIZE (64 * 1024)

void sq_write_check_remember_rom(const struct sq_cart* c, const char* path)
{
	char p[SQ_PATH_LEN], text[512];
	unsigned char* head = (unsigned char*)malloc(ROM_CHECK_SIZE);
	long size = sq_file_size(path), n = size < ROM_CHECK_SIZE ? size : ROM_CHECK_SIZE;
	FILE* fp = fopen(path, "rb");
	int len;

	if (!head || !fp || n <= 0 || (long)fread(head, 1, n, fp) != n) {
		if (fp)
			fclose(fp);
		free(head);
		return;
	}
	fclose(fp);
	len = snprintf(text, sizeof(text), "KIND=rom\nROM=%s\nROM_SIZE=%ld\nHEAD_SIZE=%ld\nHEAD_CRC=%08x\n",
		c->rom, c->rom_size, n, (unsigned)sq_crc_buffer(0, head, n));
	free(head);
	sq_mkdirs(sq_path(p, sizeof(p), SQ_HOME_DIR));
	sq_write_atomic(sq_path(p, sizeof(p), CHECK_FILE), text, len, NULL);
}

void sq_write_check_remember(const struct sq_cart* c, const void* data, long size)
{
	char path[SQ_PATH_LEN], text[512];
	int n = snprintf(text, sizeof(text), "ROM=%s\nROM_SIZE=%ld\nSAVE_SIZE=%ld\nSAVE_CRC=%08x\n",
		c->rom, c->rom_size, size, (unsigned)sq_crc_buffer(0, (const uint8_t*)data, size));
	sq_mkdirs(sq_path(path, sizeof(path), SQ_HOME_DIR));
	sq_write_atomic(sq_path(path, sizeof(path), CHECK_FILE), text, n, NULL);
}

/* One value from the record */
static int check_value(const char* text, const char* key, char* out, size_t len)
{
	const char* p = strstr(text, key);
	size_t n = 0;
	if (!p || (p != text && p[-1] != '\n'))
		return 0;
	p += strlen(key);
	while (p[n] && p[n] != '\n' && n + 1 < len) {
		out[n] = p[n];
		n++;
	}
	out[n] = '\0';
	return 1;
}

static void verdict(struct sq_result* r, int ok, const char* l1, const char* l2,
	const char* l3, const char* l4)
{
	r->ok = ok;
	snprintf(r->line[0], sizeof(r->line[0]), "%s", l1);
	snprintf(r->line[1], sizeof(r->line[1]), "%s", l2 ? l2 : "");
	snprintf(r->line[2], sizeof(r->line[2]), "%s", l3 ? l3 : "");
	snprintf(r->line[3], sizeof(r->line[3]), "%s", l4 ? l4 : "");
}

/*
 * A game was written: the slot names the cartridge's file after its game,
 * so the cartridge may come back under the new game's name. Compare the
 * start of whatever is inserted with the start of the game written. The
 * old game back: it didn't take. Another cartridge: not this check's.
 */
static int check_rom(const struct sq_cart* c, const char* text, const char* record,
	struct sq_result* r)
{
	char path[SQ_PATH_LEN], rom[256], v[32];
	unsigned char* head;
	long n;
	unsigned want_crc;
	int same_old, written = 0;
	FILE* fp;

	check_value(text, "HEAD_SIZE=", v, sizeof(v));
	n = atol(v);
	check_value(text, "HEAD_CRC=", v, sizeof(v));
	want_crc = (unsigned)strtoul(v, NULL, 16);
	if (n <= 0 || n > ROM_CHECK_SIZE || c->rom_size < n)
		return 0;
	head = (unsigned char*)malloc(n);
	sq_cart_reader_path(c, NULL, path, sizeof(path));
	fp = head ? fopen(path, "rb") : NULL;
	if (fp) {
		written = (long)fread(head, 1, n, fp) == n &&
			(unsigned)sq_crc_buffer(0, head, n) == want_crc;
		fclose(fp);
	}
	free(head);
	same_old = check_value(text, "ROM=", rom, sizeof(rom)) && !strcmp(rom, c->rom) &&
		check_value(text, "ROM_SIZE=", v, sizeof(v)) && atol(v) == c->rom_size;
	if (written) {
		verdict(r, 1, "The game is on the cartridge.",
			"The game written to it earlier was read back",
			"after the cartridge was put back in.", NULL);
		sq_log_sd("tools: checked after re-inserting: the written game is on the cartridge (%s)",
			c->rom);
	} else if (same_old) {
		verdict(r, 0, "The cartridge still has its old game.",
			"The cartridge slot took the write but the",
			"cartridge didn't keep it (most cartridges",
			"can't be rewritten).");
		sq_log_sd("tools: checked after re-inserting %s: the cartridge does NOT have the written game",
			c->rom);
	} else {
		return 0;
	}
	unlink(record);
	return 1;
}

int sq_write_check(const struct sq_cart* c, struct sq_result* r)
{
	char path[SQ_PATH_LEN], sav[SQ_PATH_LEN], rom[256], v[32], text[512];
	unsigned char* data;
	long size = 0, n;
	unsigned want_crc;
	FILE* fp;

	if (!c->present)
		return 0;
	fp = fopen(sq_path(path, sizeof(path), CHECK_FILE), "r");
	if (!fp)
		return 0;
	n = (long)fread(text, 1, sizeof(text) - 1, fp);
	fclose(fp);
	text[n > 0 ? n : 0] = '\0';

	if (check_value(text, "KIND=", v, sizeof(v)) && !strcmp(v, "rom"))
		return check_rom(c, text, path, r);

	/* Only for the cartridge it was written to */
	if (!check_value(text, "ROM=", rom, sizeof(rom)) || strcmp(rom, c->rom) != 0)
		return 0;
	if (!check_value(text, "ROM_SIZE=", v, sizeof(v)) || atol(v) != c->rom_size)
		return 0;
	check_value(text, "SAVE_CRC=", v, sizeof(v));
	want_crc = (unsigned)strtoul(v, NULL, 16);

	sq_cart_reader_path(c, ".sav", sav, sizeof(sav));
	data = sq_read_all(sav, SQ_MAX_SAVE, &size);
	if (data && (unsigned)sq_crc_buffer(0, data, size) == want_crc) {
		verdict(r, 1, "Your save is on the cartridge.",
			"The save written to it earlier was read back",
			"after the cartridge was put back in.", NULL);
		sq_log_sd("tools: checked after re-inserting %s: the written save is on the cartridge",
			c->rom);
	} else {
		verdict(r, 0, "The cartridge still has its old save.",
			"The cartridge slot took the write but didn't",
			"store it on the cartridge. Your save is safe",
			"on the SD card; the cartridge is as it was.");
		sq_log_sd("tools: checked after re-inserting %s: the cartridge does NOT have the written save",
			c->rom);
	}
	free(data);
	unlink(path);
	return 1;
}
