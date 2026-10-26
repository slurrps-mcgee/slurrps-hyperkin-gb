#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hyperkin-crc.h"

int main(int argc, char** argv){
	uint8_t buf[BLK_SIZE];
	uint32_t crc = 0x0;
	struct stat st;
	FILE* fp = NULL;
	int i = 0, count = 0, ret = 0;

	char crc_filename[1024];
	char* p = NULL;
	uint32_t crc_file_data = 0;

	if(argc <= 1){
		printf("please input rom name\n");
		return 1;
	}

	fp = fopen(argv[1], "rb");
	if(fp != NULL){
		gen_crc_table();
		stat(argv[1], &st);
		printf("[%s]file size = %ld\n", argv[1], st.st_size);

		while(count < st.st_size){
			ret = fread(buf, 1, BLK_SIZE, fp);
			if(ret <= 0) //read error or file shrank, don't spin forever
				break;
			for(i = 0; i < ret; i+=ret){
				count += ret;
				crc = update_crc(crc, buf + i, ret);
			}
		}
		printf("crc=%08x, count=%d\n", crc, count);
		fclose(fp);
	}

	strcpy(crc_filename, argv[1]);
	p = strstr(crc_filename, ".gb");
	if(p != NULL){
		strcpy(p, ".crc");
		printf("CRC Filename=[%s]\n", crc_filename);

		if(read_crc_file(crc_filename, &crc_file_data) == 0){
			printf("[%08x]\n", crc_file_data);
		}else{
			printf("crc data wrong\n");
		}
	}

	if(crc == crc_file_data){
		printf("check crc passed\n");
		ret = 0;
	}else{
		printf("check crc failed\n");
		ret = 1;
	}
	return ret;
}
