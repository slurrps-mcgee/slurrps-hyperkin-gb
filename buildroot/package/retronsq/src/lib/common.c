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
	unsigned char* data;
	long len = sq_file_size(path);
	FILE* fp;

	if (len < 0 || len > max)
		return NULL;
	fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	data = (unsigned char*)malloc(len ? len : 1);
	if (data && (long)fread(data, 1, len, fp) != len) {
		free(data);
		data = NULL;
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
	return 0;
}

int sq_same_content(const char* path, const void* data, long size)
{
	long len = 0;
	unsigned char* other = sq_read_all(path, SQ_MAX_SAVE, &len);
	int same = other && len == size && memcmp(other, data, size) == 0;
	free(other);
	return same;
}
