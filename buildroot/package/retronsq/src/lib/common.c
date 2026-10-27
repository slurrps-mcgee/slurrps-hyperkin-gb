/*
 * libsqcart: paths, errors, logging and small file helpers shared by the
 * rest of the library (see sqcart.h).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "sqcart.h"

char* sq_path(char* out, size_t len, const char* path)
{
	/* Tests point SQ_ROOT at a sandbox; on the console it is unset */
	const char* root = getenv("SQ_ROOT");
	snprintf(out, len, "%s%s", root ? root : "", path);
	return out;
}

void sq_fail(struct sq_error* e, int err, const char* fmt, ...)
{
	va_list ap;
	size_t n;

	if (!e)
		return;
	va_start(ap, fmt);
	vsnprintf(e->msg, sizeof(e->msg), fmt, ap);
	va_end(ap);
	e->err = err;
	n = strlen(e->msg);
	if (err && n < sizeof(e->msg))
		snprintf(e->msg + n, sizeof(e->msg) - n, ": %s", strerror(err));
	sq_log("%s", e->msg);
}

void sq_log(const char* fmt, ...)
{
	va_list ap;
	fprintf(stderr, "sq: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

void sq_log_sd(const char* fmt, ...)
{
	char path[SQ_PATH_LEN], up[32] = "?";
	va_list ap;
	FILE* fp;

	va_start(ap, fmt);
	fprintf(stderr, "sq: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);

	fp = fopen("/proc/uptime", "r");
	if (fp) {
		if (fscanf(fp, "%31s", up) != 1)
			strcpy(up, "?");
		fclose(fp);
	}
	sq_mkdirs(sq_path(path, sizeof(path), "/userdata/logs"));
	fp = fopen(sq_path(path, sizeof(path), "/userdata/logs/launcher.log"), "a");
	if (!fp)
		return;
	fprintf(fp, "[%s] ", up);
	va_start(ap, fmt);
	vfprintf(fp, fmt, ap);
	va_end(ap);
	fprintf(fp, "\n");
	fflush(fp);
	fsync(fileno(fp));	/* the power gets pulled: keep the line */
	fclose(fp);
}

void sq_meter_start(struct sq_meter* m, long total)
{
	m->total = total;
	m->first_ms = 0;
	m->first_bytes = 0;
}

void sq_meter_text(struct sq_meter* m, long done, unsigned long now_ms, int finished,
	char* size, size_t size_len, char* rate, size_t rate_len)
{
	if (done > m->total)
		done = m->total;
	/* Saves are KB, games MB */
	if (m->total < 1048576)
		snprintf(size, size_len, "%ld of %ld KB", (done + 1023) / 1024, (m->total + 1023) / 1024);
	else
		snprintf(size, size_len, "%.1f of %.1f MB", done / 1048576.0, m->total / 1048576.0);
	if (done > 0 && m->first_ms == 0) {
		m->first_ms = now_ms ? now_ms : 1;
		m->first_bytes = done;
	}
	if (finished) {
		snprintf(rate, rate_len, "Done");
	} else if (m->first_ms && now_ms - m->first_ms > 1500 && done > m->first_bytes) {
		double bps = (done - m->first_bytes) * 1000.0 / (now_ms - m->first_ms);
		int left = (int)((m->total - done) / bps + 0.5);
		char speed[24], when[32];
		if (bps >= 1048576.0)
			snprintf(speed, sizeof(speed), "%.1f MB/s", bps / 1048576.0);
		else
			snprintf(speed, sizeof(speed), "%ld KB/s", (long)(bps / 1024.0 + 0.5));
		if (left >= 60)
			snprintf(when, sizeof(when), "about %d min %02d s left", left / 60, left % 60);
		else
			snprintf(when, sizeof(when), "about %d s left", left < 1 ? 1 : left);
		snprintf(rate, rate_len, "%s, %s", speed, when);
	} else {
		snprintf(rate, rate_len, "Measuring speed...");
	}
}

int sq_ends_with(const char* name, const char* ext)
{
	size_t n = strlen(name), e = strlen(ext);
	return n > e && strcasecmp(name + n - e, ext) == 0;
}

long sq_file_size(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

void sq_mkdirs(const char* path)
{
	char tmp[SQ_PATH_LEN];
	char* p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(tmp, 0777);
			*p = '/';
		}
	}
	mkdir(tmp, 0777);
}

unsigned char* sq_read_all(const char* path, long max, long* size)
{
	return sq_read_all_progress(path, max, size, NULL, NULL, 0);
}

unsigned char* sq_read_all_progress(const char* path, long max, long* size,
	sq_progress_fn progress, void* ctx, long base)
{
	unsigned char* data;
	long len = sq_file_size(path), got = 0;
	FILE* fp;

	if (len < 0 || len > max)
		return NULL;
	fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	data = (unsigned char*)malloc(len ? len : 1);
	/* In 16 KB steps, so a save read from the slot (128 KB at about
	 * 1 MB/s) still shows progress */
	while (data && got < len) {
		size_t want = len - got < 16384 ? (size_t)(len - got) : 16384;
		if (fread(data + got, 1, want, fp) != want) {
			free(data);
			data = NULL;
			break;
		}
		got += want;
		if (progress)
			progress(base + got, ctx);
	}
	fclose(fp);
	if (data)
		*size = len;
	return data;
}
int sq_write_atomic(const char* path, const void* data, long size, struct sq_error* e)
{
	char tmp[SQ_PATH_LEN];
	const unsigned char* p = (const unsigned char*)data;
	long left = size;
	int fd;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) {
		sq_fail(e, errno, "creating %s", tmp);
		return -1;
	}
	while (left > 0) {
		ssize_t n = write(fd, p, left);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			sq_fail(e, n < 0 ? errno : ENOSPC, "writing %s", tmp);
			close(fd);
			unlink(tmp);
			return -1;
		}
		p += n;
		left -= n;
	}
	if (fsync(fd) != 0) {
		sq_fail(e, errno, "flushing %s", tmp);
		close(fd);
		unlink(tmp);
		return -1;
	}
	close(fd);
	if (rename(tmp, path) != 0) {
		sq_fail(e, errno, "renaming %s", tmp);
		unlink(tmp);
		return -1;
	}
	sq_sync_dir(path);
	return 0;
}

void sq_sync_dir(const char* path)
{
	char dir[SQ_PATH_LEN];
	char* slash;
	int fd;

	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash == dir)
		slash[1] = '\0';
	else if (slash)
		*slash = '\0';
	else
		snprintf(dir, sizeof(dir), ".");
	fd = open(dir, O_RDONLY | O_DIRECTORY);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
}

int sq_same_content(const char* path, const void* data, long size)
{
	long len = 0;
	unsigned char* other = sq_read_all(path, SQ_MAX_SAVE, &len);
	int same = other && len == size && memcmp(other, data, size) == 0;
	free(other);
	return same;
}

/* --- the clock ----------------------------------------------------------- */

#define CLOCK_FILE	SQ_HOME_DIR "/clock"
#define BUILD_STAMP	"/etc/retronsq-version"	/* written when the firmware is built */

int sq_clock_save(void)
{
	char path[SQ_PATH_LEN], text[32];
	int n = snprintf(text, sizeof(text), "%ld\n", (long)time(NULL));
	sq_mkdirs(sq_path(path, sizeof(path), SQ_HOME_DIR));
	return sq_write_atomic(sq_path(path, sizeof(path), CLOCK_FILE), text, n, NULL);
}

long sq_clock_wanted(void)
{
	char path[SQ_PATH_LEN];
	struct stat st;
	long saved = 0, built = 0, want;
	FILE* fp = fopen(sq_path(path, sizeof(path), CLOCK_FILE), "r");

	if (fp) {
		if (fscanf(fp, "%ld", &saved) != 1)
			saved = 0;
		fclose(fp);
	}
	if (stat(sq_path(path, sizeof(path), BUILD_STAMP), &st) == 0)
		built = (long)st.st_mtime;
	want = saved > built ? saved : built;
	return want > (long)time(NULL) ? want : 0;
}

int sq_clock_set(long t, struct sq_error* e)
{
	struct timeval tv;
	tv.tv_sec = t;
	tv.tv_usec = 0;
	if (settimeofday(&tv, NULL) != 0) {
		sq_fail(e, errno, "setting the clock");
		return -1;
	}
	sq_clock_save();
	return 0;
}
