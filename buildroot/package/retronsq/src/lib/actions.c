/*
 * Cartridge actions as the user sees them (see sqcart.h): what each does,
 * in which order, and the few lines that tell the user how it went. Shared
 * by Cartridge Tools (the RetroArch core) and the home screen, so both
 * behave and word things the same. Lines are at most SQ_RESULT_COLS
 * characters, which fits both screens.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqcart.h"

/* Set the result: ok, then up to 4 lines (NULL ends early) */
static void result(struct sq_result* r, int ok, const char* l1, const char* l2,
	const char* l3, const char* l4)
{
	const char* lines[4] = { l1, l2, l3, l4 };
	int i, end = 0;
	r->ok = ok;
	for (i = 0; i < 4; i++) {
		if (!lines[i])
			end = 1;
		snprintf(r->line[i], sizeof(r->line[i]), "%s", end ? "" : lines[i]);
	}
}

/* A failure: the headline, then the library's message over two lines,
 * broken between words */
static void failed(struct sq_result* r, const char* what, const struct sq_error* e)
{
	char l2[SQ_RESULT_COLS + 1], l3[SQ_RESULT_COLS + 1] = "";
	size_t len = strlen(e->msg), cut = len;
	if (len > SQ_RESULT_COLS) {
		for (cut = SQ_RESULT_COLS; cut > 0 && e->msg[cut] != ' '; cut--)
			;
		if (cut == 0)
			cut = SQ_RESULT_COLS;
	}
	snprintf(l2, sizeof(l2), "%.*s", (int)cut, e->msg);
	if (cut < len)
		snprintf(l3, sizeof(l3), "%s", e->msg + cut + (e->msg[cut] == ' '));
	result(r, 0, what, l2, l3[0] ? l3 : NULL, NULL);
}

/* "128 KB", "16.0 MB" */
static void size_text(long size, char* out, size_t len)
{
	if (size >= 1024 * 1024)
		snprintf(out, len, "%.1f MB", size / (1024.0 * 1024.0));
	else
		snprintf(out, len, "%ld KB", (size + 1023) / 1024);
}

int sq_save_choices(const struct sq_cart* c, int for_writing,
	struct sq_save_choice* out, int max)
{
	struct sq_backup list[64];
	char text[16], kind[32], live[SQ_PATH_LEN];
	long live_size;
	int i, n, count = 0;
	char* p;

	if (max <= 0 || !c->present)
		return 0;
	if (for_writing) {
		sq_cart_cache_path(c, ".srm", live, sizeof(live));
		live_size = sq_file_size(live);
		if (live_size > 0) {
			size_text(live_size, text, sizeof(text));
			snprintf(out[0].label, sizeof(out[0].label),
				"The save you play with now (%s)", text);
			snprintf(out[0].path, sizeof(out[0].path), "%s", live);
			count = 1;
		}
	} else if (c->save_size > 0) {
		size_text(c->save_size, text, sizeof(text));
		snprintf(out[0].label, sizeof(out[0].label), "Save on the cartridge now (%s)", text);
		sq_cart_reader_path(c, ".sav", out[0].path, sizeof(out[0].path));
		count = 1;
	}
	n = sq_backup_list(c->base, list, 64);
	for (i = 0; i < n && count < max; i++, count++) {
		/* "before-restore" -> "before restore" */
		snprintf(kind, sizeof(kind), "%s", list[i].kind);
		for (p = kind; *p; p++)
			if (*p == '-')
				*p = ' ';
		size_text(list[i].size, text, sizeof(text));
		snprintf(out[count].label, sizeof(out[count].label), "#%d %s (%s)",
			list[i].number, kind, text);
		snprintf(out[count].path, sizeof(out[count].path), "%s", list[i].path);
	}
	return count;
}

void sq_do_copy_to_library(const struct sq_cart* c, sq_progress_fn progress,
	void* ctx, int* added, struct sq_result* r)
{
	char out[SQ_PATH_LEN], line1[SQ_RESULT_COLS + 1], line2[SQ_RESULT_COLS + 1];
	struct sq_error e = { "", 0 };
	int already, verified, save_copied;
	const char* name;

	*added = 0;
	if (sq_dump_to_library(c, out, sizeof(out), &already, &verified, &save_copied,
			progress, ctx, &e) != 0) {
		failed(r, "Copy failed:", &e);
		return;
	}
	name = strrchr(out, '/') ? strrchr(out, '/') + 1 : out;
	snprintf(line1, sizeof(line1), "roms/%s/", c->sysdir);
	snprintf(line2, sizeof(line2), "%s", name);
	if (already) {
		result(r, 1, "This cartridge is already on the SD card:", line1, line2, NULL);
		return;
	}
	*added = 1;
	if (verified == SQ_VERIFIED_TWICE)
		result(r, 1, "Copied to", line1, line2, save_copied
			? "Checked by reading it twice; save copied too." : "Checked by reading it twice.");
	else if (verified)
		result(r, 1, "Copied to", line1, line2, save_copied ? "Its save was copied too." : NULL);
	else
		result(r, 0, "Copied, but two reads gave different data:", line2,
			"clean the cartridge's contacts and copy again.", NULL);
}

void sq_do_backup_save(const struct sq_cart* c, sq_progress_fn progress, void* ctx,
	struct sq_result* r)
{
	char line[SQ_RESULT_COLS + 1];
	struct sq_error e = { "", 0 };
	int existed, n = sq_backup_cart_save(c, &existed, progress, ctx, 0, &e);

	snprintf(line, sizeof(line), "saves/%s/ as #%d", c->base, n);
	if (n < 0)
		failed(r, "Backup failed:", &e);
	else if (existed)
		result(r, 1, "This save is already backed up:", line, NULL, NULL);
	else
		result(r, 1, "The cartridge's save is backed up in", line, NULL, NULL);
}

void sq_do_restore_save(const struct sq_cart* c, const char* path,
	sq_progress_fn progress, void* ctx, struct sq_result* r)
{
	struct sq_error e = { "", 0 };
	if (sq_restore_save(c, path, progress, ctx, &e) != 0)
		failed(r, "Restore failed:", &e);
	else
		result(r, 1, "Save restored.", "It is used the next time you play the cartridge.",
			"The previous save was backed up first.", NULL);
}

/* Run "<rw_cmd> <what>" (cart-event write-enable / write-disable) */
static int run_rw(const char* rw_cmd, const char* what)
{
	char cmd[SQ_PATH_LEN + 32];
	snprintf(cmd, sizeof(cmd), "%s %s", rw_cmd, what);
	return system(cmd);
}

void sq_do_write_save(const struct sq_cart* c, const char* path, const char* rw_cmd,
	sq_progress_fn progress, void* ctx, struct sq_result* r)
{
	struct sq_error e = { "", 0 };
	struct sq_gen3_report g;
	char line[SQ_RESULT_COLS + 1], a[16], b[16];
	unsigned char* data = NULL;
	long size = 0;
	int existed, n, rc;

	/* Everything that can refuse comes before anything changes */
	if (sq_reader_writable() == 0) {
		result(r, 0, "This cartridge slot can't write:",
			"it reports the cartridge as write protected.",
			"Nothing was changed.", NULL);
		return;
	}
	data = sq_read_all(path, SQ_MAX_SAVE, &size);
	if (!data) {
		sq_fail(&e, errno, "reading %s", path);
		failed(r, "Couldn't read that save:", &e);
		return;
	}
	if (size != c->save_size) {
		size_text(c->save_size, a, sizeof(a));
		size_text(size, b, sizeof(b));
		snprintf(line, sizeof(line), "The cartridge's save is %s, this one %s.", a, b);
		result(r, 0, "That save doesn't fit this cartridge.", line,
			"Nothing was changed.", NULL);
		goto done;
	}
	if (sq_is_gen3_pokemon(c->code)) {
		sq_gen3_check(data, size, &g);
		if (g.newest < 0) {
			result(r, 0, "That Pokemon save is damaged",
				"(neither of its two save slots checks out).",
				"Nothing was changed.", NULL);
			goto done;
		}
	}

	/* The cartridge's own save is kept before it is unlocked */
	n = sq_backup_cart_save_as(c, "before-write", &existed, progress, ctx, 0, &e);
	if (n < 0) {
		failed(r, "Backing up the cartridge's save failed:", &e);
		goto done;
	}
	sq_log_sd("tools: writing %s to the cartridge %s (its save is backup #%d)",
		path, c->rom, n);
	if (run_rw(rw_cmd, "write-enable") != 0) {
		sq_log_sd("tools: the cartridge couldn't be opened for writing");
		result(r, 0, "The cartridge couldn't be opened for writing.",
			"Nothing was changed. See logs/launcher.log.", NULL, NULL);
		goto done;
	}
	rc = sq_write_cart_save(c, data, size, progress, ctx, c->save_size, &e);
	if (run_rw(rw_cmd, "write-disable") != 0)
		sq_log_sd("tools: locking the cartridge again failed");
	snprintf(line, sizeof(line), "Its previous save is backup #%d.", n);
	if (rc != 0) {
		sq_log_sd("tools: writing to the cartridge failed: %s", e.msg);
		if (strstr(e.msg, "reads back different"))
			sq_cart_write_record(c, 0, 0);	/* the slot didn't take it */
		failed(r, "Writing to the cartridge failed:", &e);
		snprintf(r->line[3], sizeof(r->line[3]), "%s", line);
	} else {
		/* The slot returned it, which doesn't yet prove it reached the
		 * cartridge: that is checked when it is next inserted */
		sq_log_sd("tools: written to the cartridge and read back identical");
		sq_write_check_remember(c, data, size);
		result(r, 1, "Written to the cartridge.",
			"To check it's really on it: take the cartridge",
			"out and put it back in.", line);
	}
done:
	free(data);
}

void sq_do_write_rom(const struct sq_cart* c, const char* path, const char* rw_cmd,
	sq_progress_fn progress, void* ctx, struct sq_result* r)
{
	struct sq_error e = { "", 0 };
	char line[SQ_RESULT_COLS + 1] = "";
	int existed, n = 0, rc;

	/* Everything that can refuse comes before anything changes */
	if (sq_reader_writable() == 0) {
		result(r, 0, "This cartridge slot can't write:",
			"it reports the cartridge as write protected.",
			"Nothing was changed.", NULL);
		return;
	}
	if (sq_cart_rom_fits(c, path, &e) != 0) {
		failed(r, "That game can't go on this cartridge:", &e);
		return;
	}

	/* The cartridge's save is kept first: a new game may not keep it */
	if (c->save_size > 0) {
		n = sq_backup_cart_save_as(c, "before-write", &existed, progress, ctx, 0, &e);
		if (n < 0) {
			failed(r, "Backing up the cartridge's save failed:", &e);
			return;
		}
		snprintf(line, sizeof(line), "Its old save is backup #%d.", n);
	}
	sq_log_sd("tools: writing the game %s (%ld bytes) to the cartridge %s (%ld bytes)",
		path, sq_file_size(path), c->rom, c->rom_size);
	if (run_rw(rw_cmd, "write-enable") != 0) {
		sq_log_sd("tools: the cartridge couldn't be opened for writing");
		result(r, 0, "The cartridge couldn't be opened for writing.",
			"Nothing was changed. See logs/launcher.log.", NULL, NULL);
		return;
	}
	rc = sq_write_cart_rom(c, path, progress, ctx, c->save_size, &e);
	if (run_rw(rw_cmd, "write-disable") != 0)
		sq_log_sd("tools: locking the cartridge again failed");
	if (rc == 0) {
		/* As with a save: proven only once the cartridge comes back */
		sq_log_sd("tools: game written to the cartridge and read back identical");
		sq_write_check_remember_rom(c, path);
		result(r, 1, "Written to the cartridge.",
			"To check it took: take the cartridge out",
			"and put it back in.", line[0] ? line : NULL);
		return;
	}
	sq_log_sd("tools: writing the game to the cartridge failed: %s", e.msg);
	if (strstr(e.msg, "reads back different") && !strstr(e.msg, "part of it"))
		sq_cart_write_record(c, 1, 0);	/* the slot didn't take it */
	if (strstr(e.msg, "part of it"))
		result(r, 0, "Only part of the game was written.",
			"The cartridge may not start now; write it",
			"again, or write its old game back.", line[0] ? line : NULL);
	else if (strstr(e.msg, "reads back different"))
		result(r, 0, "The cartridge didn't take the game.",
			"It reads back different from the game: this",
			"cartridge or slot can't be rewritten.", line[0] ? line : NULL);
	else {
		failed(r, "Writing the game failed:", &e);
		if (line[0])
			snprintf(r->line[3], sizeof(r->line[3]), "%s", line);
	}
}
