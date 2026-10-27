/*
 * sqcart: the cartridge library (libsqcart) for shell scripts, so they don't
 * re-implement cartridge logic. Output is "KEY=value" lines or a short word;
 * the exit status says whether it worked.
 *
 *   sqcart info                  describe the inserted cartridge (KEY=value)
 *   sqcart reader-state          none | pending | empty | ready
 *   sqcart cache-valid [--verify]   exit 0 when the cached copy is good
 *   sqcart cache [--progress]    copy the cartridge into the cache
 *   sqcart import-save           first play: cartridge save -> live save
 *   sqcart backup-save           back up the cartridge's save (prints number)
 *   sqcart check-save FILE [CODE]   report on a save (Gen 3 Pokémon in detail)
 *   sqcart rom-header FILE       TITLE= and CODE= of a ROM file
 *   sqcart crc FILE              the reader's CRC of a file (decimal), as
 *                                stored big-endian in the cartridge's .crc
 *   sqcart history-add PATH LABEL   put a game at the top of the home
 *                                screen's "Recently played"
 *   sqcart write-check           a save was written to this cartridge
 *                                earlier: is it really on it? Prints OK=1|0
 *                                and the lines to show; exit 1 when there
 *                                was nothing to check
 *
 * See sqcart.h for what each one does underneath.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../lib/sqcart.h"

static void usage(void)
{
	fprintf(stderr,
		"usage: sqcart info | reader-state | cache-valid [--verify] | cache |\n"
		"              import-save | backup-save | check-save FILE [CODE] |\n"
		"              rom-header FILE | crc FILE | history-add PATH LABEL |\n"
		"              write-check\n");
}

/* Print a value for the shell, without characters that would break KEY=value */
static void print_kv(const char* key, const char* value)
{
	const char* p;
	printf("%s=", key);
	for (p = value ? value : ""; *p; p++)
		putchar(*p == '\n' || *p == '\r' ? ' ' : *p);
	putchar('\n');
}

/* Scan the reader; say so on stderr when there is no cartridge */
static int need_cart(struct sq_cart* c)
{
	sq_cart_scan(c);
	if (!c->present)
		fprintf(stderr, "sqcart: no cartridge\n");
	return c->present;
}

/* info: PRESENT=, then ROM=, BASE=, SYSTEM=, SYSDIR=, sizes, TITLE=, CODE= */
static int cmd_info(void)
{
	struct sq_cart c;
	char n[32];

	sq_cart_scan(&c);
	printf("PRESENT=%d\n", c.present);
	if (!c.present)
		return 1;
	print_kv("ROM", c.rom);
	print_kv("BASE", c.base);
	print_kv("SYSTEM", c.system);
	print_kv("SYSDIR", c.sysdir);
	snprintf(n, sizeof(n), "%ld", c.rom_size);
	print_kv("ROM_SIZE", n);
	snprintf(n, sizeof(n), "%ld", c.save_size);
	print_kv("SAVE_SIZE", n);
	print_kv("TITLE", c.title);
	print_kv("CODE", c.code);
	return 0;
}

/* cache-valid: 0 when /userdata/rom holds a verified copy of this
 * cartridge; --verify also recomputes the copy's CRC */
static int cmd_cache_valid(int verify)
{
	struct sq_cart c;
	char path[SQ_PATH_LEN];
	uint32_t crc, cached;

	if (!need_cart(&c) || !sq_cache_valid(&c))
		return 1;
	if (!verify)
		return 0;
	/* Re-check the copy itself against its CRC (VERIFY_CACHE=1) */
	sq_cart_cache_path(&c, NULL, path, sizeof(path));
	if (sq_file_crc(path, &crc, NULL, NULL, NULL) != 0)
		return 1;
	sq_cart_cache_path(&c, ".crc", path, sizeof(path));
	return sq_crc_read_file(path, &cached) == 0 && cached == crc ? 0 : 1;
}

/* cache: copy the cartridge into /userdata/rom (sq_cache_cartridge) */
static int cmd_cache(void)
{
	struct sq_cart c;
	struct sq_error e = { "", 0 };
	int verified;

	if (!need_cart(&c))
		return 1;
	if (sq_cache_cartridge(&c, &verified, NULL, NULL, &e) != 0) {
		fprintf(stderr, "sqcart: %s\n", e.msg);
		return 1;
	}
	printf("VERIFIED=%d\n", verified);
	return 0;
}

/* import-save: first play only, the cartridge's save becomes the live save */
static int cmd_import_save(void)
{
	struct sq_cart c;
	struct sq_error e = { "", 0 };

	if (!need_cart(&c))
		return 1;
	return sq_import_save(&c, &e) == 0 ? 0 : 1;
}

/* backup-save: BACKUP=<number> and NEW=0|1 */
static int cmd_backup_save(void)
{
	struct sq_cart c;
	struct sq_error e = { "", 0 };
	int existed, n;

	if (!need_cart(&c))
		return 1;
	n = sq_backup_cart_save(&c, &existed, &e);
	if (n < 0)
		return 1;
	printf("BACKUP=%d\nNEW=%d\n", n, n > 0 && !existed);
	return 0;
}

/* check-save: size check, and the Gen 3 slot report for Pokémon codes */
static int cmd_check_save(const char* path, const char* code)
{
	long size = 0;
	unsigned char* data = sq_read_all(path, SQ_MAX_SAVE, &size);
	struct sq_gen3_report r;
	int s;

	if (!data) {
		printf("SAVE=missing\n");
		return 1;
	}
	printf("SAVE_SIZE=%ld\nSIZE_OK=%d\n", size, sq_save_size_plausible(size));
	if (code && sq_is_gen3_pokemon(code)) {
		sq_gen3_check(data, size, &r);
		printf("GEN3_TRUNCATED=%d\n", r.truncated);
		for (s = 0; s < 2; s++)
			printf("GEN3_SLOT%c=%s counter=%u sections=%d bad_checksums=%d bad_signatures=%d\n",
				'A' + s, r.slot[s].valid ? "valid" : "INVALID", r.slot[s].counter,
				r.slot[s].sections, r.slot[s].bad_checksums, r.slot[s].bad_signatures);
		printf("GEN3_LOADS=%s\n", r.newest < 0 ? "none" : r.newest ? "B" : "A");
	}
	free(data);
	return 0;
}

int main(int argc, char** argv)
{
	const char* cmd = argc > 1 ? argv[1] : "";

	if (!strcmp(cmd, "info"))
		return cmd_info();
	if (!strcmp(cmd, "reader-state")) {
		printf("%s\n", sq_reader_state_name(sq_reader_state()));
		return 0;
	}
	if (!strcmp(cmd, "cache-valid"))
		return cmd_cache_valid(argc > 2 && !strcmp(argv[2], "--verify"));
	if (!strcmp(cmd, "cache"))
		return cmd_cache();
	if (!strcmp(cmd, "import-save"))
		return cmd_import_save();
	if (!strcmp(cmd, "backup-save"))
		return cmd_backup_save();
	if (!strcmp(cmd, "check-save") && argc > 2)
		return cmd_check_save(argv[2], argc > 3 ? argv[3] : NULL);
	if (!strcmp(cmd, "rom-header") && argc > 2) {
		char title[17], code[5];
		sq_rom_header(argv[2], title, code);
		print_kv("TITLE", title);
		print_kv("CODE", code);
		return 0;
	}
	if (!strcmp(cmd, "write-check")) {
		struct sq_cart c;
		struct sq_result r;
		int i;
		sq_cart_scan(&c);
		if (!sq_write_check(&c, &r))
			return 1;
		printf("OK=%d\n", r.ok);
		for (i = 0; i < 4; i++)
			if (r.line[i][0])
				printf("%s\n", r.line[i]);
		return 0;
	}
	if (!strcmp(cmd, "history-add") && argc > 3)
		return sq_history_add(argv[2], argv[3]) == 0 ? 0 : 1;
	if (!strcmp(cmd, "crc") && argc > 2) {
		struct sq_error e;
		uint32_t crc;
		if (sq_file_crc(argv[2], &crc, NULL, NULL, &e) != 0)
			return 1;
		printf("%u\n", (unsigned)crc);
		return 0;
	}
	usage();
	return 2;
}
