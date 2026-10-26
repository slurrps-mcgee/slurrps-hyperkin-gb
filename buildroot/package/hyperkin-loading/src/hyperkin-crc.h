#ifndef HYPERKIN_CRC_H
#define HYPERKIN_CRC_H

#include <stdint.h>
#include <stdio.h>

/*
 * The CRC the cartridge reader writes to its .crc file: CRC-32 (0x04C11DB7,
 * not reflected) fed in 512 byte blocks, inverted after every block.
 * Shared by hyperkin-crc and hyperkin-loading so both always agree.
 */
#define POLYNOMIAL		0x04C11DB7L
#define BLK_SIZE 512

static uint32_t crc_table[256];

static void gen_crc_table(void){
	uint16_t i, j;
	uint32_t crc_accum;

	for ( i=0; i < 256; i++ ){
		crc_accum = (uint32_t) (i << 24);
		for ( j=0; j < 8; j++ ){
			if ( crc_accum & 0x80000000L )
				crc_accum = (crc_accum << 1) ^ POLYNOMIAL;
			else
				crc_accum = (crc_accum << 1);
		}
		crc_table[i] = crc_accum;
	}
}

static uint32_t update_crc(uint32_t crc_accum, uint8_t *data_blk_ptr, uint32_t data_blk_size){
	uint32_t i, j;
	for ( j=0; j < data_blk_size; j++ ){
		i = ((int32_t) (crc_accum >> 24) ^ *data_blk_ptr++) & 0xFF;
		crc_accum = (crc_accum << 8) ^ crc_table[i];
	}
	crc_accum = ~crc_accum;
	return crc_accum;
} // update_crc()

/* The .crc file holds the value big endian. Returns 0 on success. */
static int read_crc_file(const char* path, uint32_t* crc){
	uint8_t buf[4];
	FILE* fp = fopen(path, "rb");
	int ret = -1;

	if(fp == NULL)
		return -1;
	if(fread(buf, 1, 4, fp) == 4){
		*crc = (uint32_t)buf[0] << 24 | buf[1] << 16 | buf[2] << 8 | buf[3];
		ret = 0;
	}
	fclose(fp);
	return ret;
}

#endif
