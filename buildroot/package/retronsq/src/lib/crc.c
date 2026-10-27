/*
 * The cartridge reader's checksum, as written to its <name>.crc file:
 * CRC-32 with polynomial 0x04C11DB7, not reflected, fed in 512-byte blocks,
 * with the running value inverted after every block. That last quirk makes
 * it unlike any standard CRC-32, so it lives here once and everything (the
 * loader, Cartridge Tools, sqcart) uses this copy.
 */
#include <pthread.h>
#include <stdio.h>

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
