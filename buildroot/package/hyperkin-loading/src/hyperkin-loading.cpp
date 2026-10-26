#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_mixer.h>
#include <iostream>
#include <vector>
#include <stdlib.h> //rand()
#include <unistd.h> //usleep
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <stdio.h>
#include <math.h>
#include <getopt.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <asm/param.h>
#include <linux/joystick.h>
#include <dirent.h>
#include <sys/stat.h>

#include "hyperkin-crc.h"

#define PIC_LOADING "/etc/Splash_Screen_MockUp.jpg"
#ifndef DUMPER_PATH
#define DUMPER_PATH "/media/usb0"
#define USERDATA_ROM_PATH "/userdata/rom"
#define USERDATA_PATH "/userdata"
#endif
#define MIN_SPLASH_MS 500

/*
 * ROMs are copied through a ring of large buffers: one thread reads the
 * cartridge (and computes its CRC on the way), another writes the SD card,
 * so the USB reads and SD writes overlap instead of taking turns.
 */
#define SLOT_SIZE (256 * 1024) /* multiple of the 512 byte CRC block */
#define SLOT_COUNT 8

long int g_dumper_total_size = 0;
long int g_avail_size = 0;
volatile long int copied_total_size = 0;
pthread_mutex_t copy_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile int copy_thread_exit = 0;
int copy_failed = 0;

struct slot { unsigned char data[SLOT_SIZE]; ssize_t len; };
static struct slot* ring = NULL;
static int ring_count = 0, ring_reader_done = 0;
static volatile int ring_write_failed = 0;
static pthread_mutex_t ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ring_cond = PTHREAD_COND_INITIALIZER;

SDL_Texture* copyright = NULL;
int WIDTH=1280;
int HEIGHT=720;

void ListDrivers(void){
	printf("Testing video drivers...\n");
	std::vector< bool > drivers( SDL_GetNumVideoDrivers() );
	for( int i = 0; i < drivers.size(); ++i ){
		drivers[ i ] = ( 0 == SDL_VideoInit( SDL_GetVideoDriver( i ) ) );
		SDL_VideoQuit();
	}

	printf("SDL_VIDEODRIVER available:");
	for( int i = 0; i < drivers.size(); ++i ){
		printf(" %s", SDL_GetVideoDriver(i));
	}
	printf("\n");

	printf("SDL_VIDEODRIVER usable   :");
	for( int i = 0; i < drivers.size(); ++i ){
		if( !drivers[ i ] ) continue;
			printf(" %s", SDL_GetVideoDriver(i));
	}
	printf("\n");
}

void ListDisplayMode(void){
	int disp_index = 0, i = 0;
	int numDisplayModes = SDL_GetNumDisplayModes(disp_index);
	SDL_DisplayMode displaymode;
	int got = 0;
	
	printf("Number of display modes=%d\n", numDisplayModes);
	for(i = 0; i < numDisplayModes; i++){
		if(SDL_GetDisplayMode(disp_index, i, &displaymode) >= 0){
			printf("%dx%d-%d\n", displaymode.w, displaymode.h, displaymode.refresh_rate);
			if(got == 0){//we use first one resolution.
				WIDTH = displaymode.w;
				HEIGHT = displaymode.h;
				got = 1;
			}
		}
	}
}

static int EndsWith(const char* name, const char* ext){
	size_t n = strlen(name), e = strlen(ext);
	return n > e && strcasecmp(name + n - e, ext) == 0;
}

static int IsRom(const char* name){
	return EndsWith(name, ".gb") || EndsWith(name, ".gbc") || EndsWith(name, ".gba");
}

static inline int sortbysize(const struct dirent **a, const struct dirent **b){
	int rval;
	struct stat sbuf1, sbuf2;
	char path1[1024], path2[1024];
	//printf("path=[%s], a[%s], b[%s]\n", USERDATA_ROM_PATH, (*a)->d_name, (*b)->d_name);
	sprintf(path1, "%s/%s", USERDATA_ROM_PATH, (*a)->d_name);
	sprintf(path2, "%s/%s", USERDATA_ROM_PATH, (*b)->d_name);

	rval = stat(path1, &sbuf1);
	if(rval != 0){
		printf("stat error[%s]\n", path1); return 0;
	}
	rval = stat(path2, &sbuf2);
	if(rval != 0){
		printf("stat error[%s]\n", path2); return 0;
	}

	//smallest first, so walking the list backwards deletes the largest first
	return (sbuf1.st_size > sbuf2.st_size) - (sbuf1.st_size < sbuf2.st_size);
}

void CleanUserdata(void){
	DIR* pdir;
	struct dirent **pdirent;
	int n = 0, ret = 0;
	struct stat sbuf;
	char path[1024];
	long int avail_size = g_avail_size;
	pdir = opendir(USERDATA_ROM_PATH);
	if(pdir){
		n = scandir(USERDATA_ROM_PATH, &pdirent, NULL, sortbysize);
		printf("n=%d\n", n);
		while(n--){
			if(!strcmp(pdirent[n]->d_name, ".") || !strcmp(pdirent[n]->d_name, ".."))
				goto free_and_continue;
			//only cached ROMs can be fetched again, keep saves and states
			if(!IsRom(pdirent[n]->d_name) && !EndsWith(pdirent[n]->d_name, ".part"))
				goto free_and_continue;
			sprintf(path, "%s/%s", USERDATA_ROM_PATH, pdirent[n]->d_name);
			stat(path, &sbuf);
			printf("%s[%ld]\n", pdirent[n]->d_name, sbuf.st_size);
			if(avail_size < g_dumper_total_size){
				printf("delete [%s]\n", path);
				ret = remove(path);
				sync();
				avail_size += sbuf.st_size;
			}
free_and_continue:
			free(pdirent[n]);
		}
		free(pdirent);
	}
}

#include <sys/statvfs.h>
long GetAvailableSpace(const char* path){
	struct statvfs stData;

	if((statvfs(path,&stData)) < 0 ) {
		printf("Failed to stat %s:\n", path);
		return -1;
	}else{
		printf("Disk [%s]: available size=%ld bytes\n", path, stData.f_bavail * stData.f_bsize);
		return stData.f_bavail * stData.f_bsize;
	}
}

long int CheckFiles(const char* dumper_folder){
	struct dirent *dir = NULL;
	DIR* di = opendir(dumper_folder);
	long int total_size = 0;
	struct stat st;
	if(di){
		while((dir = readdir(di)) != NULL){
			if(IsRom(dir->d_name) || EndsWith(dir->d_name, ".sav") || EndsWith(dir->d_name, ".crc")){
				char full_name[1024];
				snprintf(full_name, sizeof(full_name), "%s/%s", dumper_folder, dir->d_name);
				if(stat(full_name, &st) != 0)
					continue;
				total_size += st.st_size;
				printf("found name[%s], size[%ld], total[%ld]\n", dir->d_name, st.st_size, total_size);
			}
		}
		closedir(di);
	}
	return total_size;
}

static int CopyFile(const char* src_path, const char* src_name, const char* dst_path, const char* dst_name){
	char src_full_name[1024], dst_full_name[1024];
	int len = 0, src_file = 0, dst_file = 0, ret = 0;
	char buffer[1024];
	sprintf(src_full_name, "%s/%s", src_path, src_name);
	sprintf(dst_full_name, "%s/%s", dst_path, dst_name);

	src_file = open(src_full_name, O_RDONLY | O_NOCTTY);
	if(src_file == -1) { printf("open [%s] fail\n", src_full_name); return -1; }
	else printf("open [%s] success, start read\n", src_full_name);
	dst_file = open(dst_full_name, O_WRONLY | O_CREAT | O_TRUNC, 0777);
	if(dst_file == -1) { printf("open [%s] fail\n", dst_full_name); close(src_file); return -1; }
	else printf("open [%s] success, start write\n", dst_full_name);
	while (1){//copy file
		char *p, *q;
		len = read(src_file, buffer, sizeof(buffer));
		if(len == 0){//end of file
			break;
		}else{
			if(len < 0){
				printf("[%s]read error: %s\n", src_full_name, strerror(errno));
				ret = -1;
				goto next_file1;
			}
		}
		p = buffer;
		q = buffer + len;
		while ( p < q ){
			len = write(dst_file, p, (size_t)(q - p));
			if( len > 0){
				p += len;
				pthread_mutex_lock(&copy_mutex);
				copied_total_size += len;
				pthread_mutex_unlock(&copy_mutex);
			}else{
				printf("[%s]write error: %s\n", dst_full_name, strerror(errno));
				ret = -1;
				goto next_file1;
			}
		}
	}
next_file1:
	fsync(dst_file);fdatasync(dst_file);
	close(dst_file);
	close(src_file);
	if(ret < 0) { printf("remove [%s] file\n", dst_full_name); remove(dst_full_name); }
	printf("Copy to [%s] done\n", dst_full_name);
	return ret;
}

static int CheckSrm(const char* old_path, const char *old_name){
	char old_full_name[1024];
	char new_name[1024];
	char* p = NULL;
	int ret = 0;

	sprintf(old_full_name, "%s/%s", old_path, old_name);
	strcpy(new_name, old_full_name);
	p = strstr(new_name, ".sav");
	if(p != NULL){
		struct stat st;
		strcpy(p, ".srm");
		if(access(new_name, F_OK) != -1){//file exists
			stat(new_name, &st);
			copied_total_size += st.st_size;
			printf("%s file exists, so do not copy\n", new_name);
			return 1;
		}else{//file doesn't exist
			printf("%s file doesn't exists, so copy it\n", new_name);
			return 0;
		}
	}
	return 0;
}

static int SavRenameSrm(const char* old_path, const char *old_name){
	char old_full_name[1024];
	char new_name[1024];
	char* p = NULL;
	sprintf(old_full_name, "%s/%s", old_path, old_name);
	strcpy(new_name, old_full_name);
	p = strstr(new_name, ".sav");
	if(p != NULL){
		strcpy(p, ".srm");
		rename(old_full_name, new_name);
		system("sync");
	}
	return 0;
}

static ssize_t ReadFull(int fd, unsigned char* buf, size_t size){
	size_t got = 0;
	while(got < size){
		ssize_t len = read(fd, buf + got, size - got);
		if(len == 0)
			break;
		if(len < 0){
			if(errno == EINTR) continue;
			return -1;
		}
		got += len;
	}
	return got;
}

static void* WriterThread(void* ptr){
	int dst_file = *(int*)ptr;
	int tail = 0;

	for(;;){
		pthread_mutex_lock(&ring_lock);
		while(ring_count == 0 && !ring_reader_done)
			pthread_cond_wait(&ring_cond, &ring_lock);
		if(ring_count == 0){ //reader finished and everything is written
			pthread_mutex_unlock(&ring_lock);
			break;
		}
		pthread_mutex_unlock(&ring_lock);

		struct slot* s = &ring[tail];
		unsigned char* p = s->data;
		ssize_t left = s->len;
		while(left > 0 && !ring_write_failed){
			ssize_t len = write(dst_file, p, left);
			if(len < 0 && errno == EINTR) continue;
			if(len <= 0){
				printf("write error: %s\n", strerror(errno));
				ring_write_failed = 1;
				break;
			}
			p += len;
			left -= len;
			pthread_mutex_lock(&copy_mutex);
			copied_total_size += len;
			pthread_mutex_unlock(&copy_mutex);
		}

		//keep draining after a failure so the reader never blocks
		pthread_mutex_lock(&ring_lock);
		tail = (tail + 1) % SLOT_COUNT;
		ring_count--;
		pthread_cond_broadcast(&ring_cond);
		pthread_mutex_unlock(&ring_lock);
	}
	return NULL;
}

/* Copy a ROM to <name>.part while computing its CRC. Returns 0 on success. */
static int CopyRom(const char* name, uint32_t* crc_out){
	char src_full_name[1024], dst_full_name[1024];
	int src_file, dst_file, head = 0, read_failed = 0;
	uint32_t crc = 0;
	pthread_t writer;

	snprintf(src_full_name, sizeof(src_full_name), "%s/%s", DUMPER_PATH, name);
	snprintf(dst_full_name, sizeof(dst_full_name), "%s/%s.part", USERDATA_ROM_PATH, name);

	src_file = open(src_full_name, O_RDONLY | O_NOCTTY);
	if(src_file == -1) { printf("open [%s] fail\n", src_full_name); return -1; }
	dst_file = open(dst_full_name, O_WRONLY | O_CREAT | O_TRUNC, 0777);
	if(dst_file == -1) { printf("open [%s] fail\n", dst_full_name); close(src_file); return -1; }
	posix_fadvise(src_file, 0, 0, POSIX_FADV_SEQUENTIAL);
	printf("copy [%s] -> [%s]\n", src_full_name, dst_full_name);

	ring_count = 0;
	ring_reader_done = 0;
	ring_write_failed = 0;
	if(pthread_create(&writer, NULL, WriterThread, &dst_file) != 0){
		close(dst_file); close(src_file); remove(dst_full_name);
		return -1;
	}

	for(;;){
		pthread_mutex_lock(&ring_lock);
		while(ring_count == SLOT_COUNT)
			pthread_cond_wait(&ring_cond, &ring_lock);
		pthread_mutex_unlock(&ring_lock);
		if(ring_write_failed)
			break;

		struct slot* s = &ring[head];
		s->len = ReadFull(src_file, s->data, SLOT_SIZE);
		if(s->len < 0){
			printf("[%s]read error: %s\n", src_full_name, strerror(errno));
			read_failed = 1;
			break;
		}
		if(s->len == 0)
			break;

		//same 512 byte blocks as hyperkin-crc, only the very last one is short
		for(ssize_t off = 0; off < s->len; off += BLK_SIZE){
			ssize_t blk = s->len - off < BLK_SIZE ? s->len - off : BLK_SIZE;
			crc = update_crc(crc, s->data + off, blk);
		}

		pthread_mutex_lock(&ring_lock);
		head = (head + 1) % SLOT_COUNT;
		ring_count++;
		pthread_cond_broadcast(&ring_cond);
		pthread_mutex_unlock(&ring_lock);

		if(s->len < SLOT_SIZE) //end of file
			break;
	}

	pthread_mutex_lock(&ring_lock);
	ring_reader_done = 1;
	pthread_cond_broadcast(&ring_cond);
	pthread_mutex_unlock(&ring_lock);
	pthread_join(writer, NULL);

	fdatasync(dst_file);
	close(dst_file);
	close(src_file);
	if(read_failed || ring_write_failed){
		printf("remove [%s] file\n", dst_full_name);
		remove(dst_full_name);
		return -1;
	}
	*crc_out = crc;
	return 0;
}

/*
 * Copy one ROM and check it against the reader's .crc. A bad read is retried
 * once; if it still doesn't match (common with reproduction carts) the ROM is
 * kept so the game still starts, but without a cached .crc so the next boot
 * reads the cartridge again instead of trusting this copy.
 */
static int CopyRomVerified(const char* name){
	char base[1024], crc_name[1024], part_name[1024], final_name[1024];
	char* dot;
	uint32_t crc = 0, expected = 0;
	long int progress_start = copied_total_size;
	int have_crc, verified = 0, attempt;

	snprintf(base, sizeof(base), "%s", name);
	dot = strrchr(base, '.');
	if(dot) *dot = '\0';
	snprintf(crc_name, sizeof(crc_name), "%s/%s.crc", DUMPER_PATH, base);
	snprintf(part_name, sizeof(part_name), "%s/%s.part", USERDATA_ROM_PATH, name);
	snprintf(final_name, sizeof(final_name), "%s/%s", USERDATA_ROM_PATH, name);
	have_crc = read_crc_file(crc_name, &expected) == 0;

	for(attempt = 0; attempt < 2; attempt++){
		copied_total_size = progress_start;
		if(CopyRom(name, &crc) != 0)
			continue;
		if(!have_crc){
			printf("no .crc from the reader, can't verify [%s]\n", name);
			break;
		}
		if(crc == expected){
			verified = 1;
			break;
		}
		printf("crc mismatch [%s]: read %08x, reader says %08x\n", name, crc, expected);
	}
	if(access(part_name, F_OK) != 0)
		return -1;

	//only a complete copy ever gets the real name
	snprintf(crc_name, sizeof(crc_name), "%s.crc", base);
	if(rename(part_name, final_name) != 0){
		printf("rename [%s] fail: %s\n", part_name, strerror(errno));
		return -1;
	}
	if(verified){
		printf("crc check passed [%08x]\n", crc);
		CopyFile(DUMPER_PATH, crc_name, USERDATA_ROM_PATH, crc_name);
	}else{
		char cached_crc[1024];
		snprintf(cached_crc, sizeof(cached_crc), "%s/%s", USERDATA_ROM_PATH, crc_name);
		remove(cached_crc);
	}
	return 0;
}

static void* CopyThread(void* ptr){
	struct dirent *dir = NULL;
	DIR* di = opendir(DUMPER_PATH);
	copy_thread_exit = 0;

	printf("CopyThread Start\n");
	gen_crc_table();
	ring = (struct slot*)malloc(sizeof(struct slot) * SLOT_COUNT);
	if(ring == NULL || di == NULL)
		copy_failed = 1;
	if(!copy_failed){
		while((dir = readdir(di)) != NULL){//process gb/gba/gbc/sav
			if(IsRom(dir->d_name)){
				if(CopyRomVerified(dir->d_name) != 0)
					copy_failed = 1;
			}
			if(EndsWith(dir->d_name, ".sav") && CheckSrm(USERDATA_ROM_PATH, dir->d_name) == 0){
				if(CopyFile(DUMPER_PATH, dir->d_name, USERDATA_ROM_PATH, dir->d_name) == 0)
					SavRenameSrm(USERDATA_ROM_PATH, dir->d_name);
			}
		}
	}
	if(di)
		closedir(di);
	free(ring);
	ring = NULL;
	sync();
	printf("CopyThread Exit\n");
	copy_thread_exit = 1;
	return NULL;
}

static void DrawLoading(SDL_Renderer* renderer, float loading_x_ratio, float loading_y_ratio){
	long int copied;
	pthread_mutex_lock(&copy_mutex);
	copied = copied_total_size;
	pthread_mutex_unlock(&copy_mutex);
	if(copied > g_dumper_total_size)
		copied = g_dumper_total_size;

	//Draw hyperkin loading background picture
	SDL_RenderCopyEx(renderer, copyright, NULL, NULL, 0, NULL, SDL_FLIP_NONE);

	//Draw progress bar
	SDL_SetRenderDrawColor(renderer, 255, 255, 0, 255);
	SDL_Rect rect;
	rect.x = 645 * loading_x_ratio; rect.y = 624 * loading_y_ratio;
	rect.w = g_dumper_total_size > 0 ?
		(1508 - 645) * loading_x_ratio * ((float)copied / (float)g_dumper_total_size) : 0;
	rect.h = (656 - 624) * loading_y_ratio;
	SDL_RenderFillRect(renderer, &rect);

	SDL_RenderPresent( renderer );
}

int main( int argc, char** argv ){
	pthread_t copy_thread;
	int copy_started = 0;

	putenv((char*)"SDL_MONITOR_INDEX=1");
	printf("SDL_MONITOR_INDEX=1\n");
	g_dumper_total_size = CheckFiles(DUMPER_PATH);
	printf("Check Dumper Files : Total ROM and SAV Size=[%ld]\n", g_dumper_total_size);
	g_avail_size = GetAvailableSpace(USERDATA_PATH);

	if(g_avail_size > 0 && g_avail_size < g_dumper_total_size){
		CleanUserdata();
		GetAvailableSpace(USERDATA_PATH);
	}

	//Start reading the cartridge right away, the splash screen (EGL, SDL,
	//JPEG decode) gets set up while the first megabytes are already copying
	mkdir(USERDATA_ROM_PATH, 0777);
	if(pthread_create(&copy_thread, NULL, CopyThread, NULL) == 0){
		copy_started = 1;
	}else{
		printf("CopyThread Failed\n");
		return EXIT_FAILURE;
	}

	SDL_Window* window = NULL;
	SDL_Renderer* renderer = NULL;
	if( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_EVENTS ) < 0 ){
		printf("SDL_Init(): %s\n", SDL_GetError());
		goto wait_copy;
	}
	printf("SDL_VIDEODRIVER selected : %s\n", SDL_GetCurrentVideoDriver());
	ListDisplayMode();
	window = SDL_CreateWindow("SDL2", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		WIDTH, HEIGHT, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL);
	if( NULL == window ){
		printf("SDL_CreateWindow(): %s\n", SDL_GetError());
		goto wait_copy;
	}

	renderer = SDL_CreateRenderer( window, -1, SDL_RENDERER_ACCELERATED );
	if( NULL == renderer ){
		printf("SDL_CreateRenderer(): %s\n", SDL_GetError());
		goto wait_copy;
	}

	{
		SDL_RendererInfo info;
		SDL_GetRendererInfo( renderer, &info );
		printf("SDL_RENDER_DRIVER selected : %s\n", info.name);

		int flags = IMG_INIT_JPG | IMG_INIT_PNG;
		int initted = IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
		if((initted&flags) != flags) {
			SDL_Log("IMG_Init: Failed to init required jpg and png support!\n");
			SDL_Log("IMG_Init: %s\n", IMG_GetError());
		}
		copyright = IMG_LoadTexture(renderer, PIC_LOADING);
		float loading_x_ratio = WIDTH / 1920.0;
		float loading_y_ratio = HEIGHT / 1080.0;

		Uint32 start_time = SDL_GetTicks();
		bool running = true;
		while( running ){
			SDL_Event ev;
			while( SDL_PollEvent( &ev ) ){
				if(( ev.type == SDL_QUIT ) || ( ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE )){
					running = false;
				}
			}
			DrawLoading(renderer, loading_x_ratio, loading_y_ratio);

			if(copy_thread_exit == 1) {
				DrawLoading(renderer, loading_x_ratio, loading_y_ratio);
				//keep the finished bar on screen briefly instead of flashing
				if((SDL_GetTicks() - start_time) < MIN_SPLASH_MS)
					SDL_Delay(MIN_SPLASH_MS - (SDL_GetTicks() - start_time));
				running = false;
				break;
			}
			SDL_Delay(16);
		}
	}

wait_copy:
	if(copy_started)
		pthread_join(copy_thread, NULL);
	if(renderer) SDL_DestroyRenderer( renderer );
	if(window) SDL_DestroyWindow( window );
	SDL_Quit();
	return copy_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
