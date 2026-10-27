/*
 * Writing a save to the cartridge: the one thing in the firmware that ever
 * changes a cartridge, for Cartridge Tools' "Write a save to the
 * cartridge" (see sqcart.h for the API, sqtools.c for the steps around it).
 *
 * The reader shows the cartridge as a small FAT drive. Whether it passes
 * writes to the save file on to the save chip depends on the reader's own
 * firmware, so:
 *   - the drive's write protection, as reported when it was mounted, is
 *     respected (sq_reader_writable);
 *   - the save file is overwritten in place with exactly its own size:
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

/* Read size bytes of path, bypassing the page cache: O_DIRECT (aligned
 * buffer, whole 512-byte blocks), or where that isn't supported, dropping
 * the file's cached pages first. Returns a buffer of at least size bytes. */
static unsigned char* read_uncached(const char* path, long size, struct sq_error* e)
{
	long aligned = (size + 511) / 512 * 512, got = 0;
	unsigned char* buf = NULL;
	int fd = open(path, O_RDONLY | O_DIRECT);

	if (fd < 0 && errno == EINVAL) {
		fd = open(path, O_RDONLY);
		if (fd >= 0)
			posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
	}
	if (fd < 0) {
		sq_fail(e, errno, "reading the cartridge's save back");
		return NULL;
	}
	if (posix_memalign((void**)&buf, 4096, aligned) != 0) {
		close(fd);
		sq_fail(e, ENOMEM, "reading the cartridge's save back");
		return NULL;
	}
	while (got < size) {
		ssize_t n = read(fd, buf + got, aligned - got);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EINVAL && got == 0) {
			/* O_DIRECT refused at read time: plain read, cache dropped */
			close(fd);
			fd = open(path, O_RDONLY);
			if (fd < 0)
				break;
			posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
			continue;
		}
		if (n <= 0)
			break;
		got += n;
	}
	if (fd >= 0)
		close(fd);
	if (got < size) {
		sq_fail(e, errno, "reading the cartridge's save back");
		free(buf);
		return NULL;
	}
	return buf;
}

int sq_write_cart_save(const struct sq_cart* c, const void* data, long size,
	struct sq_error* e)
{
	char path[SQ_PATH_LEN];
	const unsigned char* p = (const unsigned char*)data;
	unsigned char* back;
	long done = 0;
	int fd, same;

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

	/* In place: no O_TRUNC, no O_CREAT, no temporary file and rename */
	sq_cart_reader_path(c, ".sav", path, sizeof(path));
	fd = open(path, O_WRONLY);
	if (fd < 0) {
		sq_fail(e, errno, "opening the cartridge's save for writing");
		return -1;
	}
	while (done < size) {
		ssize_t n = pwrite(fd, p + done, size - done, done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			sq_fail(e, n < 0 ? errno : EIO, "writing the cartridge's save");
			close(fd);
			return -1;
		}
		done += n;
	}
	if (fsync(fd) != 0) {
		sq_fail(e, errno, "writing the cartridge's save");
		close(fd);
		return -1;
	}
	close(fd);

	back = read_uncached(path, size, e);
	if (!back)
		return -1;
	same = memcmp(back, data, size) == 0;
	free(back);
	if (!same) {
		sq_fail(e, 0, "the cartridge reads back different data");
		return -1;
	}
	return 0;
}

/* --- did the write stick? -------------------------------------------------- */

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
