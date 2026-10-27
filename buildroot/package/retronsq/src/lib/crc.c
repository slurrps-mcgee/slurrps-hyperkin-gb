/*
 * The cartridge reader's checksum, as written to its <name>.crc file:
 * CRC-32 with polynomial 0x04C11DB7, not reflected, fed in 512-byte blocks,
 * with the running value inverted after every block. That last quirk makes
 * it unlike any standard CRC-32, so it lives here once and everything (the
 * loader, Cartridge Tools, sqcart) uses this copy.
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sqcart.h"

#define POLYNOMIAL 0x04C11DB7u

static uint32_t crc_table[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;

static void crc_init(void)
{
	unsigned i, j;
	for (i = 0; i < 256; i++) {
		uint32_t c = (uint32_t)i << 24;
		for (j = 0; j < 8; j++)
			c = (c & 0x80000000u) ? (c << 1) ^ POLYNOMIAL : (c << 1);
		crc_table[i] = c;
	}
}

/* One block: len is 512, or less for the last block of a file */
uint32_t sq_crc_update(uint32_t crc, const uint8_t* data, size_t len)
{
	size_t i;
	pthread_once(&crc_once, crc_init);
	for (i = 0; i < len; i++)
		crc = (crc << 8) ^ crc_table[((crc >> 24) ^ data[i]) & 0xFF];
	return ~crc;
}

uint32_t sq_crc_buffer(uint32_t crc, const uint8_t* data, size_t len)
{
	size_t off;
	for (off = 0; off < len; off += SQ_CRC_BLOCK)
		crc = sq_crc_update(crc, data + off,
			len - off < SQ_CRC_BLOCK ? len - off : SQ_CRC_BLOCK);
	return crc;
}

int sq_crc_read_file(const char* path, uint32_t* crc)
{
	uint8_t b[4];
	FILE* fp = fopen(path, "rb");
	int ok;

	if (!fp)
		return -1;
	ok = fread(b, 1, 4, fp) == 4;
	fclose(fp);
	if (!ok)
		return -1;
	*crc = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
	return 0;
}

/* --- which checksum is the slot's? ----------------------------------------- */

/*
 * Every genuine cartridge tried so far mismatched the slot's .crc with the
 * algorithm above (copied from the stock firmware's hyperkin-crc, whose
 * result the stock firmware never showed). So after a mismatch the copy is
 * checked against the usual CRC-32 variants, in one pass, and the one that
 * matches (if any) is logged: block-inverting with other block sizes,
 * plain/MPEG-2/BZIP2 (MSB first), zlib/JAMCRC (reflected), POSIX cksum,
 * each against the .crc read big and little endian.
 */
enum kind { BLOCKS, MSB, LSB, POSIX };
struct variant {
	const char* name;
	enum kind kind;
	uint32_t init, xorout;
	long block;		/* BLOCKS: inverted after every block of this size */
	uint32_t crc;
	long in_block;
};

static uint32_t lsb_table[256];
static pthread_once_t lsb_once = PTHREAD_ONCE_INIT;

static void lsb_init(void)
{
	unsigned i, j;
	for (i = 0; i < 256; i++) {
		uint32_t c = i;
		for (j = 0; j < 8; j++)
			c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
		lsb_table[i] = c;
	}
}

static uint32_t msb_bytes(uint32_t crc, const uint8_t* p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		crc = (crc << 8) ^ crc_table[((crc >> 24) ^ p[i]) & 0xFF];
	return crc;
}

static void variant_feed(struct variant* v, const uint8_t* p, size_t n)
{
	size_t i;
	switch (v->kind) {
	case BLOCKS:
		while (n > 0) {
			size_t take = (size_t)(v->block - v->in_block) < n ? (size_t)(v->block - v->in_block) : n;
			v->crc = msb_bytes(v->crc, p, take);
			v->in_block += take;
			p += take;
			n -= take;
			if (v->in_block == v->block) {
				v->crc = ~v->crc;
				v->in_block = 0;
			}
		}
		break;
	case MSB:
	case POSIX:
		v->crc = msb_bytes(v->crc, p, n);
		break;
	case LSB:
		for (i = 0; i < n; i++)
			v->crc = (v->crc >> 8) ^ lsb_table[(v->crc ^ p[i]) & 0xFF];
		break;
	}
}

static uint32_t variant_end(struct variant* v, long size)
{
	if (v->kind == BLOCKS && v->in_block)
		v->crc = ~v->crc;	/* the last, short block */
	if (v->kind == POSIX) {
		uint8_t b;
		unsigned long len = (unsigned long)size;
		for (; len; len >>= 8) {
			b = len & 0xFF;
			v->crc = msb_bytes(v->crc, &b, 1);
		}
	}
	return v->crc ^ v->xorout;
}

static uint32_t swap32(uint32_t x)
{
	return (x >> 24) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) | (x << 24);
}

int sq_crc_probe(const char* path, uint32_t expected, char* name, size_t len)
{
	struct variant v[] = {
		{ "slot-512", BLOCKS, 0, 0, 512, 0, 0 },
		{ "slot-1k", BLOCKS, 0, 0, 1024, 0, 0 },
		{ "slot-2k", BLOCKS, 0, 0, 2048, 0, 0 },
		{ "slot-4k", BLOCKS, 0, 0, 4096, 0, 0 },
		{ "slot-8k", BLOCKS, 0, 0, 8192, 0, 0 },
		{ "slot-16k", BLOCKS, 0, 0, 16384, 0, 0 },
		{ "slot-32k", BLOCKS, 0, 0, 32768, 0, 0 },
		{ "slot-64k", BLOCKS, 0, 0, 65536, 0, 0 },
		{ "slot-512-init-ffffffff", BLOCKS, 0xFFFFFFFFu, 0, 512, 0, 0 },
		{ "crc32-msb-plain", MSB, 0, 0, 0, 0, 0 },
		{ "crc32-mpeg2", MSB, 0xFFFFFFFFu, 0, 0, 0, 0 },
		{ "crc32-bzip2", MSB, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0, 0 },
		{ "crc32-zlib", LSB, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0, 0 },
		{ "crc32-jamcrc", LSB, 0xFFFFFFFFu, 0, 0, 0, 0 },
		{ "crc32-lsb-plain", LSB, 0, 0, 0, 0, 0 },
		{ "crc32-posix", POSIX, 0, 0xFFFFFFFFu, 0, 0, 0 },
	};
	size_t nv = sizeof(v) / sizeof(v[0]), i;
	unsigned char* buf = (unsigned char*)malloc(65536);
	long size = 0;
	ssize_t n;
	int fd, found = 0;

	pthread_once(&crc_once, crc_init);
	pthread_once(&lsb_once, lsb_init);
	if (name && len)
		name[0] = '\0';
	fd = open(path, O_RDONLY);
	if (fd < 0 || !buf) {
		if (fd >= 0)
			close(fd);
		free(buf);
		return -1;
	}
	for (i = 0; i < nv; i++)
		v[i].crc = v[i].init;
	while ((n = read(fd, buf, 65536)) > 0) {
		for (i = 0; i < nv; i++)
			variant_feed(&v[i], buf, (size_t)n);
		size += n;
	}
	close(fd);
	free(buf);
	if (n < 0)
		return -1;
	for (i = 0; i < nv && !found; i++) {
		uint32_t c = variant_end(&v[i], size);
		if (c == expected || swap32(c) == expected) {
			found = 1;
			if (name)
				snprintf(name, len, "%s%s", v[i].name, c == expected ? "" : " (little endian)");
		}
	}
	return found;
}
