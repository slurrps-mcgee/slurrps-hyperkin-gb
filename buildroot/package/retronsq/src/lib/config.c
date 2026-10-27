/*
 * Settings: reading and changing retronsq.cfg on the SD card (see
 * sqcart.h). The file is edited on PCs too, so reading is as tolerant as
 * the scripts' CfgGet (common.sh), and writing keeps everything else in the
 * file, comments included. Defaults come from defaults.cfg, which the
 * scripts read as well.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqcart.h"

#define MAX_CFG (64 * 1024)

/* If line sets key ("KEY=value", spaces allowed around both), return the
 * start of the value, else NULL */
static const char* match_key(const char* line, const char* key)
{
	size_t n = strlen(key);
	while (*line == ' ' || *line == '\t')
		line++;
	if (strncmp(line, key, n) != 0)
		return NULL;
	line += n;
	while (*line == ' ' || *line == '\t')
		line++;
	if (*line != '=')
		return NULL;
	return line + 1;
}

/* The value from the last line setting key in path, cleaned like CfgGet:
 * quotes, spaces, tabs and \r removed. 1 when found with a value. */
static int read_value(const char* path, const char* key, char* out, size_t len)
{
	char line[512];
	int found = 0;
	FILE* fp = fopen(path, "r");

	if (!fp)
		return 0;
	while (fgets(line, sizeof(line), fp)) {
		const char* v = match_key(line, key);
		size_t o = 0;
		if (!v)
			continue;
		for (; *v && *v != '\n' && o + 1 < len; v++)
			if (!strchr("\"' \t\r", *v))
				out[o++] = *v;
		out[o] = '\0';
		found = o > 0;
	}
	fclose(fp);
	return found;
}

char* sq_cfg_get(const char* key, char* out, size_t len)
{
	char path[SQ_PATH_LEN];

	out[0] = '\0';
	if (read_value(sq_path(path, sizeof(path), SQ_CFG_FILE), key, out, len))
		return out;
	if (!read_value(sq_path(path, sizeof(path), SQ_DEFAULTS_FILE), key, out, len))
		out[0] = '\0';
	return out;
}

int sq_cfg_set(const char* key, const char* value, struct sq_error* e)
{
	char path[SQ_PATH_LEN], line[512];
	char* buf = (char*)malloc(MAX_CFG);
	size_t used = 0;
	int done = 0, rc;
	const char* p;
	FILE* fp;

	/* Keys and values go into a file the scripts read: keep them plain */
	for (p = key; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '_') {
			sq_fail(e, 0, "bad setting name %s", key);
			free(buf);
			return -1;
		}
	for (p = value; *p; p++)
		if (!isalnum((unsigned char)*p) && !strchr("_-.", *p)) {
			sq_fail(e, 0, "bad value for %s", key);
			free(buf);
			return -1;
		}
	if (!buf) {
		sq_fail(e, 0, "out of memory");
		return -1;
	}

	fp = fopen(sq_path(path, sizeof(path), SQ_CFG_FILE), "r");
	while (fp && fgets(line, sizeof(line), fp)) {
		size_t n;
		if (match_key(line, key)) {
			if (done)
				continue;	/* a duplicate: the new line replaces all */
			snprintf(line, sizeof(line), "%s=%s\n", key, value);
			done = 1;
		}
		/* Windows line endings stay what they were */
		n = strlen(line);
		if (used + n >= MAX_CFG)
			break;
		memcpy(buf + used, line, n);
		used += n;
	}
	if (fp)
		fclose(fp);
	if (!done) {
		if (used > 0 && buf[used - 1] != '\n' && used < MAX_CFG)
			buf[used++] = '\n';
		used += snprintf(buf + used, MAX_CFG - used, "%s=%s\n", key, value);
	}
	rc = sq_write_atomic(path, buf, used, e);
	free(buf);
	return rc;
}
