/*
 * Sandbox helpers shared by the C tests (test_sqcart.c, test_sqtools.c):
 * a fake console under $SQ_ROOT with a mounted cartridge reader, the way
 * libsqcart sees one (files in /media/cart, the disk in sysfs, the mount in
 * /proc/mounts). Each test program includes this once.
 */
#ifndef SQ_TEST_SANDBOX_H
#define SQ_TEST_SANDBOX_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sqcart.h"

static int failures = 0, checks = 0;
static char root[512];

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* --- sandbox helpers ---------------------------------------------------- */

static void path(char* out, const char* rel) { snprintf(out, SQ_PATH_LEN, "%s%s", root, rel); }

static void put(const char* rel, const void* data, long size)
{
	char p[SQ_PATH_LEN], dir[SQ_PATH_LEN];
	FILE* fp;
	path(p, rel);
	snprintf(dir, sizeof(dir), "%s", p);
	*strrchr(dir, '/') = '\0';
	sq_mkdirs(dir);
	fp = fopen(p, "wb");
	fwrite(data, 1, size, fp);
	fclose(fp);
}

static void put_text(const char* rel, const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	put(rel, buf, strlen(buf));
}

static int exists(const char* rel) { char p[SQ_PATH_LEN]; path(p, rel); return access(p, F_OK) == 0; }
static long size_of(const char* rel) { char p[SQ_PATH_LEN]; path(p, rel); return sq_file_size(p); }

static unsigned char* random_data(long size, unsigned seed)
{
	unsigned char* d = malloc(size);
	long i;
	srand(seed);
	for (i = 0; i < size; i++)
		d[i] = rand() & 0xFF;
	return d;
}

/* The stock hyperkin-crc algorithm, independently: fread 512 at a time */
static uint32_t stock_crc(const unsigned char* data, long size)
{
	static uint32_t table[256];
	uint32_t crc = 0;
	long off;
	int i, j;
	for (i = 0; i < 256; i++) {
		uint32_t c = (uint32_t)i << 24;
		for (j = 0; j < 8; j++)
			c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
		table[i] = c;
	}
	for (off = 0; off < size; off += 512) {
		long n = size - off < 512 ? size - off : 512;
		for (i = 0; i < n; i++)
			crc = (crc << 8) ^ table[((crc >> 24) ^ data[off + i]) & 0xFF];
		crc = ~crc;
	}
	return crc;
}

static void put_crc(const char* rel, uint32_t crc)
{
	unsigned char b[4] = { crc >> 24, crc >> 16, crc >> 8, crc };
	put(rel, b, 4);
}

/* A mounted cartridge: reader files, sysfs disk and /proc/mounts entry */
static void insert_cart(const char* name, long rom_size, long save_size, unsigned seed,
	int good_crc, unsigned generation)
{
	char rel[256], base[128];
	unsigned char* rom = random_data(rom_size, seed);
	char* dot;

	/* GBA header: title at 0xA0, code at 0xAC */
	if (rom_size > 0x150) {
		memcpy(rom + 0xA0, "POKEMON LEAF", 12);
		memcpy(rom + 0xAC, "BPGE", 4);
	}
	snprintf(base, sizeof(base), "%s", name);
	dot = strrchr(base, '.');
	*dot = '\0';
	snprintf(rel, sizeof(rel), "/media/cart/%s", name);
	put(rel, rom, rom_size);
	snprintf(rel, sizeof(rel), "/media/cart/%s.crc", base);
	put_crc(rel, good_crc ? stock_crc(rom, rom_size) : 0xDEADBEEF);
	if (save_size) {
		unsigned char* sav = random_data(save_size, seed + 1);
		snprintf(rel, sizeof(rel), "/media/cart/%s.sav", base);
		put(rel, sav, save_size);
		free(sav);
	}
	put_text("/sys/class/block/sda/size", "%ld\n", (rom_size + 511) / 512 + 64);
	put_text("/proc/mounts", "/dev/sda %s/media/cart vfat ro 0 0\n", root);
	put_text("/run/retronsq/cart-generation", "%u\n", generation);
	free(rom);
}

static void remove_cart(void)
{
	char cmd[SQ_PATH_LEN * 2];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s/media/cart' '%s/sys/class/block/sda'", root, root);
	if (system(cmd) != 0)
		fprintf(stderr, "failed: %s\n", cmd);
	put_text("/proc/mounts", "");
}

static void clean(void)
{
	char cmd[SQ_PATH_LEN * 2];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s'", root, root);
	if (system(cmd) != 0)
		fprintf(stderr, "failed: %s\n", cmd);
}

/* A sandbox file as a NUL-terminated string (free it), or NULL */
static __attribute__((unused)) char* read_text(const char* rel)
{
	char p[SQ_PATH_LEN];
	long size = 0;
	unsigned char* data;
	char* text;
	path(p, rel);
	data = sq_read_all(p, 1024 * 1024, &size);
	if (!data)
		return NULL;
	text = (char*)malloc(size + 1);
	if (text) {
		memcpy(text, data, size);
		text[size] = '\0';
	}
	free(data);
	return text;
}

/* --- Gen 3 Pokémon saves ------------------------------------------------ */

static void le32w(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* Write a valid slot: 14 sectors, rotated like the game does */
static void make_slot(uint8_t* save, int slot, uint32_t counter, unsigned seed)
{
	int i;
	srand(seed);
	for (i = 0; i < 14; i++) {
		uint8_t* sec = save + (slot * 14 + i) * 0x1000;
		int id = (i + counter) % 14, j;
		uint32_t sum = 0;
		int size = id == 0 ? 3884 : id == 4 ? 3848 : id == 13 ? 2000 : 3968;
		memset(sec, 0, 0x1000);
		for (j = 0; j < size; j++)
			sec[j] = rand() & 0xFF;
		for (j = 0; j < 3968; j += 4)
			sum += sec[j] | sec[j + 1] << 8 | sec[j + 2] << 16 | (uint32_t)sec[j + 3] << 24;
		sec[0xFF4] = id;
		sec[0xFF6] = (uint16_t)((sum >> 16) + sum) & 0xFF;
		sec[0xFF7] = (uint16_t)((sum >> 16) + sum) >> 8;
		le32w(sec + 0xFF8, 0x08012025u);
		le32w(sec + 0xFFC, counter);
	}
}

/* Point libsqcart at the sandbox: $SQ_ROOT, or DEFAULT when it isn't set */
static void sandbox_init(const char* fallback)
{
	snprintf(root, sizeof(root), "%s", getenv("SQ_ROOT") ? getenv("SQ_ROOT") : fallback);
	setenv("SQ_ROOT", root, 1);
	clean();
}

#endif
