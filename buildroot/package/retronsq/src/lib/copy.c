/*
 * Fast, safe file copy for cartridge ROMs.
 *
 * The cartridge slot reads at USB 1.1 speed (about 1 MB/s), and SD card
 * writes take time too, so the copy runs on two threads joined by a ring of
 * large buffers: this thread reads (and computes the reader's CRC on the
 * way), a writer thread writes. Neither waits for the other unless the ring
 * is full or empty.
 *
 * The data goes to <dst>.part, renamed to <dst> only once complete and
 * flushed, so an interrupted copy never leaves a truncated file behind
 * under the real name.
 */
#define _GNU_SOURCE	/* O_DIRECT */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sqcart.h"

#define SLOT_SIZE (256 * 1024)	/* multiple of the 512-byte CRC block */
#define SLOT_COUNT 8

struct slot {
	unsigned char data[SLOT_SIZE];
	ssize_t len;
};

/* One copy in progress: the ring and its bookkeeping */
struct ring {
	struct slot* slots;
	int count;		/* filled slots waiting to be written */
	int reader_done;	/* no more slots will be filled */
	int write_errno;	/* set by the writer on failure */
	int dst;		/* destination file descriptor */
	long written;
	sq_progress_fn progress;
	void* ctx;
	pthread_mutex_t lock;
	pthread_cond_t cond;
};

/* read() until buf is full or the file ends; -1 on error */
static ssize_t read_full(int fd, unsigned char* buf, size_t size)
{
	size_t got = 0;
	while (got < size) {
		ssize_t n = read(fd, buf + got, size - got);
		if (n == 0)
			break;
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		got += n;
	}
	return got;
}

/* Writer side: write filled slots oldest first until the reader is done */
static void* writer_thread(void* arg)
{
	struct ring* r = (struct ring*)arg;
	int tail = 0;

	for (;;) {
		struct slot* s;
		unsigned char* p;
		ssize_t left;

		pthread_mutex_lock(&r->lock);
		while (r->count == 0 && !r->reader_done)
			pthread_cond_wait(&r->cond, &r->lock);
		if (r->count == 0) {
			pthread_mutex_unlock(&r->lock);
			break;
		}
		pthread_mutex_unlock(&r->lock);

		/* The slot at tail is full and won't be touched by the reader
		 * until handed back below, so write it without the lock */
		s = &r->slots[tail];
		p = s->data;
		left = s->len;
		while (left > 0 && !r->write_errno) {
			ssize_t n = write(r->dst, p, left);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				r->write_errno = n < 0 ? errno : ENOSPC;
				break;
			}
			p += n;
			left -= n;
			r->written += n;
			if (r->progress)
				r->progress(r->written, r->ctx);
		}

		/* Keep draining after a failure so the reader never blocks */
		pthread_mutex_lock(&r->lock);
		tail = (tail + 1) % SLOT_COUNT;
		r->count--;
		pthread_cond_broadcast(&r->cond);
		pthread_mutex_unlock(&r->lock);
	}
	return NULL;
}

int sq_copy(const char* src, const char* dst, uint32_t* crc,
	sq_progress_fn progress, void* ctx, struct sq_error* e)
{
	char part[SQ_PATH_LEN];
	struct ring r;
	pthread_t writer;
	int in, head = 0, read_errno = 0;
	uint32_t sum = 0;

	memset(&r, 0, sizeof(r));
	r.progress = progress;
	r.ctx = ctx;
	snprintf(part, sizeof(part), "%s.part", dst);

	in = open(src, O_RDONLY);
	if (in < 0) {
		sq_fail(e, errno, "opening %s", src);
		return -1;
	}
	r.dst = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (r.dst < 0) {
		sq_fail(e, errno, "creating %s", part);
		close(in);
		return -1;
	}
	/* The cartridge is read start to end: let the kernel read further ahead */
	posix_fadvise(in, 0, 0, POSIX_FADV_SEQUENTIAL);

	r.slots = (struct slot*)malloc(sizeof(struct slot) * SLOT_COUNT);
	pthread_mutex_init(&r.lock, NULL);
	pthread_cond_init(&r.cond, NULL);
	if (!r.slots || pthread_create(&writer, NULL, writer_thread, &r) != 0) {
		sq_fail(e, ENOMEM, "starting the copy");
		free(r.slots);
		close(r.dst);
		close(in);
		unlink(part);
		return -1;
	}

	for (;;) {
		struct slot* s;

		/* Wait for a free slot (the writer is a full ring behind) */
		pthread_mutex_lock(&r.lock);
		while (r.count == SLOT_COUNT)
			pthread_cond_wait(&r.cond, &r.lock);
		pthread_mutex_unlock(&r.lock);
		if (r.write_errno)
			break;

		s = &r.slots[head];
		s->len = read_full(in, s->data, SLOT_SIZE);
#ifdef SQ_TEST_READ_DELAY_US
		usleep(SQ_TEST_READ_DELAY_US);	/* tests: imitate the slow slot */
		if (getenv("SQ_TEST_STALL") && head == 2)
			pause();		/* tests: a cartridge that stops answering */
#endif
		if (s->len < 0) {
			read_errno = errno;
			break;
		}
		if (s->len == 0)
			break;
		/* Slots are whole 512-byte blocks, so CRC blocks line up with the
		 * reader's; only the file's very last block is short */
		sum = sq_crc_buffer(sum, s->data, s->len);

		/* Hand the filled slot to the writer */
		pthread_mutex_lock(&r.lock);
		head = (head + 1) % SLOT_COUNT;
		r.count++;
		pthread_cond_broadcast(&r.cond);
		pthread_mutex_unlock(&r.lock);

		if (s->len < SLOT_SIZE)
			break;
	}

	/* No more data: the writer exits once the ring is empty */
	pthread_mutex_lock(&r.lock);
	r.reader_done = 1;
	pthread_cond_broadcast(&r.cond);
	pthread_mutex_unlock(&r.lock);
	pthread_join(writer, NULL);

	if (!read_errno && !r.write_errno && fsync(r.dst) != 0)
		r.write_errno = errno;
	close(r.dst);
	close(in);
	free(r.slots);
	pthread_mutex_destroy(&r.lock);
	pthread_cond_destroy(&r.cond);

	if (read_errno || r.write_errno) {
		if (read_errno)
			sq_fail(e, read_errno, "reading %s", src);
		else
			sq_fail(e, r.write_errno, "writing %s", part);
		unlink(part);
		return -1;
	}
	if (rename(part, dst) != 0) {
		sq_fail(e, errno, "renaming %s", part);
		unlink(part);
		return -1;
	}
	sq_sync_dir(dst);
	if (crc)
		*crc = sum;
	return 0;
}

int sq_file_crc(const char* path, uint32_t* crc, sq_progress_fn progress,
	void* ctx, struct sq_error* e)
{
	unsigned char* buf = (unsigned char*)malloc(SLOT_SIZE);
	uint32_t sum = 0;
	ssize_t len;
	long done = 0;
	int fd = open(path, O_RDONLY);

	if (fd < 0 || !buf) {
		sq_fail(e, fd < 0 ? errno : ENOMEM, "opening %s", path);
		if (fd >= 0)
			close(fd);
		free(buf);
		return -1;
	}
	while ((len = read_full(fd, buf, SLOT_SIZE)) > 0) {
		sum = sq_crc_buffer(sum, buf, len);
		done += len;
		if (progress)
			progress(done, ctx);
	}
	if (len < 0)
		sq_fail(e, errno, "reading %s", path);
	close(fd);
	free(buf);
	if (len < 0)
		return -1;
	*crc = sum;
	return 0;
}

/* --- reading the cartridge again, for real --------------------------------- */

int sq_open_uncached(const char* path, int* direct)
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

void sq_drop_cache(const char* path)
{
	int fd = open(path, O_RDONLY);
	if (fd >= 0) {
		posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
		close(fd);
	}
}

#define SAMPLES 32
#define SAMPLE_SIZE (32 * 1024)

int sq_sample_check(const char* cart, const char* copy, long size)
{
	unsigned char *a = NULL, *b = (unsigned char*)malloc(SAMPLE_SIZE);
	int direct, i, same = 1, fd = sq_open_uncached(cart, &direct), cfd = open(copy, O_RDONLY);

	if (fd < 0 || cfd < 0 || !b || posix_memalign((void**)&a, 4096, SAMPLE_SIZE) != 0) {
		same = -1;
		goto out;
	}
	for (i = 0; i < SAMPLES && same == 1; i++) {
		/* Spread over the whole game, on 4 KB boundaries (O_DIRECT) */
		long off = size > SAMPLE_SIZE ? (size - SAMPLE_SIZE) / (SAMPLES - 1) * i / 4096 * 4096 : 0;
		long len = size - off < SAMPLE_SIZE ? size - off : SAMPLE_SIZE;
		ssize_t got = pread(fd, a, direct ? (len + 511) / 512 * 512 : len, off);
		if (got < 0 && errno == EINVAL && direct && i == 0) {
			/* O_DIRECT refused at read time: plain reads, cache dropped */
			close(fd);
			sq_drop_cache(cart);
			fd = open(cart, O_RDONLY);
			direct = 0;
			if (fd < 0) {
				same = -1;
				break;
			}
			got = pread(fd, a, len, off);
		}
		if (got < len || pread(cfd, b, len, off) != len)
			same = -1;
		else if (memcmp(a, b, len) != 0)
			same = 0;
	}
out:
	if (fd >= 0)
		close(fd);
	if (cfd >= 0)
		close(cfd);
	free(a);
	free(b);
	return same;
}
