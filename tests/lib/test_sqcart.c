/*
 * Unit tests for libsqcart, run against a sandbox ($SQ_ROOT) with a fake
 * reader, SD card and sysfs (sandbox.h). Built and run by tests/run-all.sh.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sqcart.h"
#include "sandbox.h"

/* --- tests -------------------------------------------------------------- */

static void test_crc(void)
{
	long sizes[] = { 1000, 512, 262144, 262144 * 3 + 123, 5242999 };
	unsigned i;
	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		unsigned char* d = random_data(sizes[i], i);
		CHECK(sq_crc_buffer(0, d, sizes[i]) == stock_crc(d, sizes[i]));
		free(d);
	}
}

static void test_reader_state(void)
{
	clean();
	CHECK(sq_reader_state() == SQ_READER_NONE);
	put_text("/sys/bus/usb/devices/3-1/idVendor", "0000\n");
	CHECK(sq_reader_state() == SQ_READER_PENDING);		/* reader, no disk yet */
	put_text("/sys/bus/usb/devices/3-1/3-1:1.1/host0/target0:0:0/0:0:0:0/block/sda/size", "0\n");
	put_text("/sys/class/block/sda/size", "0\n");
	CHECK(sq_reader_state() == SQ_READER_EMPTY);		/* no medium */
	put_text("/sys/class/block/sda/size", "4096\n");
	CHECK(sq_reader_state() == SQ_READER_PENDING);		/* medium, not mounted */
	put_text("/proc/mounts", "/dev/sda %s/media/cart vfat ro 0 0\n", root);
	CHECK(sq_reader_state() == SQ_READER_READY);
	/* Hot-swap leftovers: still listed as mounted, but the device is gone */
	put_text("/sys/class/block/sda/size", "0\n");
	CHECK(sq_reader_state() == SQ_READER_EMPTY);
}

static void test_scan_and_cache(void)
{
	struct sq_cart c;
	struct sq_error e = { "", 0 };
	int verified;

	clean();
	insert_cart("Pokemon_LeafGreen.gba", 3 * 262144 + 777, 131072, 1, 1, 7);
	sq_cart_scan(&c);
	CHECK(c.present);
	CHECK(!strcmp(c.rom, "Pokemon_LeafGreen.gba"));
	CHECK(!strcmp(c.base, "Pokemon_LeafGreen"));
	CHECK(!strcmp(c.sysdir, "gba"));
	CHECK(!strcmp(c.title, "POKEMON LEAF"));
	CHECK(!strcmp(c.code, "BPGE"));
	CHECK(c.save_size == 131072);
	CHECK(c.generation == 7);
	CHECK(!sq_cache_valid(&c));

	CHECK(sq_cache_cartridge(&c, &verified, NULL, NULL, &e) == 0);
	CHECK(verified);
	CHECK(size_of("/userdata/rom/Pokemon_LeafGreen.gba") == c.rom_size);
	CHECK(exists("/userdata/rom/Pokemon_LeafGreen.crc"));
	CHECK(!exists("/userdata/rom/Pokemon_LeafGreen.gba.part"));
	CHECK(sq_cache_valid(&c));

	/* Swapped while the console was on: the scan is stale, copies refuse */
	put_text("/run/retronsq/cart-generation", "8\n");
	CHECK(!sq_cart_still_there(&c));
	CHECK(sq_cache_cartridge(&c, &verified, NULL, NULL, &e) != 0);
	CHECK(strstr(e.msg, "removed or changed") != NULL);

	/* Repro cart whose CRC never matches: copy kept, not trusted */
	clean();
	insert_cart("Repro.gba", 262144, 0, 2, 0, 1);
	sq_cart_scan(&c);
	CHECK(sq_cache_cartridge(&c, &verified, NULL, NULL, &e) == 0);
	CHECK(!verified);
	CHECK(exists("/userdata/rom/Repro.gba"));
	CHECK(!exists("/userdata/rom/Repro.crc"));
	CHECK(!sq_cache_valid(&c));

	/* Cartridge pulled: nothing to scan */
	remove_cart();
	sq_cart_scan(&c);
	CHECK(!c.present);
}

static void test_saves(void)
{
	struct sq_cart c;
	struct sq_error e = { "", 0 };
	struct sq_backup list[16];
	int existed, n;
	unsigned char* other;
	char p[SQ_PATH_LEN];

	clean();
	insert_cart("Game.gba", 4096, 131072, 3, 1, 1);
	sq_cart_scan(&c);

	/* First play imports the cartridge save; later never overwrites it */
	CHECK(sq_import_save(&c, &e) == 0);
	CHECK(size_of("/userdata/rom/Game.srm") == 131072);
	other = random_data(131072, 99);
	put("/userdata/rom/Game.srm", other, 131072);
	CHECK(sq_import_save(&c, &e) == 0);
	path(p, "/userdata/rom/Game.srm");
	CHECK(sq_same_content(p, other, 131072));

	/* Backups: numbered, never stored twice */
	n = sq_backup_cart_save(&c, &existed, &e);
	CHECK(n == 1 && !existed);
	n = sq_backup_cart_save(&c, &existed, &e);
	CHECK(n == 1 && existed);
	put("/userdata/saves/Game/009-before-restore.sav", "x", 1);
	put("/userdata/saves/Game/010-cartridge.sav", "y", 1);
	n = sq_backup_store("Game", "new", 3, "cartridge", &existed, &e);
	CHECK(n == 11 && !existed);	/* 009/010: no octal surprises */
	CHECK(sq_backup_list("Game", list, 16) == 4);
	CHECK(list[0].number == 11 && list[3].number == 1);
	CHECK(!strcmp(list[1].kind, "cartridge") && !strcmp(list[2].kind, "before-restore"));

	/* Restore backup #1: the live save becomes it, the old one is backed up */
	CHECK(sq_restore_save(&c, list[3].path, &e) == 0);
	path(p, "/media/cart/Game.sav");
	{
		long sz;
		unsigned char* sav = sq_read_all(p, SQ_MAX_SAVE, &sz);
		path(p, "/userdata/rom/Game.srm");
		CHECK(sav && sq_same_content(p, sav, sz));
		free(sav);
	}
	CHECK(exists("/userdata/saves/Game/012-before-restore.sav"));
	path(p, "/userdata/saves/Game/012-before-restore.sav");
	CHECK(sq_same_content(p, other, 131072));
	free(other);
}

static void test_dump_to_library(void)
{
	struct sq_cart c;
	struct sq_error e = { "", 0 };
	char out[SQ_PATH_LEN];
	int already, verified, save_copied;

	clean();
	insert_cart("Game.gba", 262144 + 5, 8192, 4, 1, 1);
	sq_cart_scan(&c);
	CHECK(sq_dump_to_library(&c, out, sizeof(out), &already, &verified, &save_copied,
		NULL, NULL, &e) == 0);
	CHECK(!already && verified && save_copied);
	CHECK(size_of("/userdata/roms/gba/Game.gba") == c.rom_size);
	CHECK(size_of("/userdata/roms/gba/Game.srm") == 8192);

	CHECK(sq_dump_to_library(&c, out, sizeof(out), &already, &verified, &save_copied,
		NULL, NULL, &e) == 0);
	CHECK(already);

	/* A different game with the same name */
	insert_cart("Game.gba", 262144 + 5, 0, 5, 1, 2);
	sq_cart_scan(&c);
	CHECK(sq_dump_to_library(&c, out, sizeof(out), &already, &verified, &save_copied,
		NULL, NULL, &e) == 0);
	CHECK(!already && strstr(out, "Game (2).gba"));
}

/* --- Gen 3 save checker ------------------------------------------------- */

static void test_gen3(void)
{
	uint8_t* save = calloc(1, 131072);
	struct sq_gen3_report r;

	CHECK(sq_is_gen3_pokemon("BPGE") && sq_is_gen3_pokemon("AXVJ") && !sq_is_gen3_pokemon("AGSE"));
	make_slot(save, 0, 5, 1);
	make_slot(save, 1, 6, 2);
	sq_gen3_check(save, 131072, &r);
	CHECK(r.slot[0].valid && r.slot[1].valid && r.newest == 1 && !r.truncated);

	save[14 * 0x1000 + 100] ^= 0xFF;	/* damage the newest slot */
	sq_gen3_check(save, 131072, &r);
	CHECK(!r.slot[1].valid && r.slot[1].bad_checksums == 1 && r.newest == 0);

	make_slot(save, 1, 6, 2);
	sq_gen3_check(save, 65536, &r);		/* reader read one bank only */
	CHECK(r.truncated && r.slot[0].valid && !r.slot[1].valid && r.newest == 0);
	free(save);
}

/* Writing a save to the cartridge (Cartridge Tools, experimental) */
static void test_cart_write(void)
{
	struct sq_cart c;
	struct sq_error e;
	unsigned char* data = random_data(131072, 42);
	unsigned char* other = random_data(65536, 43);
	char p[SQ_PATH_LEN];

	clean();
	CHECK(sq_reader_writable() == -1);		/* not recorded: unknown */
	put_text("/run/retronsq/reader-wp", "1\n");
	CHECK(sq_reader_writable() == 0);
	put_text("/run/retronsq/reader-wp", "0\n");
	CHECK(sq_reader_writable() == 1);

	insert_cart("Game.gba", 1024 * 1024, 131072, 7, 1, 1);
	sq_cart_scan(&c);
	path(p, "/media/cart/Game.sav");

	/* Another size: refused, the cartridge's save untouched */
	CHECK(sq_write_cart_save(&c, other, 65536, &e) != 0 && strstr(e.msg, "65536"));
	CHECK(size_of("/media/cart/Game.sav") == 131072 && !sq_same_content(p, data, 131072));

	/* Same size: written in place and read back */
	CHECK(sq_write_cart_save(&c, data, 131072, &e) == 0);
	CHECK(sq_same_content(p, data, 131072));
	CHECK(!exists("/media/cart/Game.sav.tmp") && !exists("/media/cart/Game.sav.part"));

	/* A read-only cartridge file (not opened for writing): a clear error */
	chmod(p, 0444);
	CHECK(sq_write_cart_save(&c, data, 131072, &e) != 0 && strstr(e.msg, "opening"));
	chmod(p, 0644);

	/* Swapped since the scan: refused */
	put_text("/run/retronsq/cart-generation", "9\n");
	CHECK(sq_write_cart_save(&c, data, 131072, &e) != 0 && strstr(e.msg, "removed or changed"));

	/* No save on the cartridge: refused */
	remove_cart();
	insert_cart("NoSave.gba", 1024 * 1024, 0, 8, 1, 10);
	sq_cart_scan(&c);
	CHECK(sq_write_cart_save(&c, data, 131072, &e) != 0 && strstr(e.msg, "no save"));

	/* Did it stick? Checked when the cartridge is next inserted: here a
	 * cartridge that came back holding what was written */
	{
		struct sq_result r;
		remove_cart();
		insert_cart("Game.gba", 1024 * 1024, 131072, 7, 1, 12);
		put("/media/cart/Game.sav", data, 131072);
		sq_cart_scan(&c);
		CHECK(sq_write_check(&c, &r) == 0);		/* nothing remembered */
		sq_write_check_remember(&c, data, 131072);
		/* The slot kept it: the cartridge comes back with the new save */
		CHECK(sq_write_check(&c, &r) == 1 && r.ok && strstr(r.line[0], "is on the cartridge"));
		CHECK(!exists("/userdata/home/cart-write-check"));	/* checked once */
		/* The slot didn't: the old save comes back */
		sq_write_check_remember(&c, other, 65536);
		CHECK(sq_write_check(&c, &r) == 1 && !r.ok && strstr(r.line[0], "still has its old save"));
		/* Another cartridge: not its check */
		sq_write_check_remember(&c, data, 131072);
		remove_cart();
		insert_cart("Other.gba", 1024 * 1024, 131072, 9, 1, 11);
		sq_cart_scan(&c);
		CHECK(sq_write_check(&c, &r) == 0 && exists("/userdata/home/cart-write-check"));
	}

	/* sq_log_sd keeps a line on the SD card */
	sq_log_sd("test line %d", 7);
	{
		char* log = read_text("/userdata/logs/launcher.log");
		CHECK(log && strstr(log, "] test line 7\n"));
		free(log);
	}
	free(data);
	free(other);
}

/* Settings: tolerant reading like CfgGet, writing that keeps the file */
static void test_config(void)
{
	char v[64];
	char* text;
	struct sq_error e;

	clean();
	put_text("/usr/share/retronsq/defaults.cfg", "# defaults\nGBA_CORE=gpsp\nCART_WAIT=25\n");
	CHECK(!strcmp(sq_cfg_get("GBA_CORE", v, sizeof(v)), "gpsp"));	/* no file */
	put_text("/userdata/retronsq.cfg",
		"# my settings\r\nGBA_CORE = \"mgba\"\r\nCART_WAIT=\nGBA_CORE_X=1\nGBA_CORE=vbam\n");
	CHECK(!strcmp(sq_cfg_get("GBA_CORE", v, sizeof(v)), "vbam"));	/* last wins */
	CHECK(!strcmp(sq_cfg_get("CART_WAIT", v, sizeof(v)), "25"));	/* empty: default */
	CHECK(!strcmp(sq_cfg_get("NOPE", v, sizeof(v)), ""));

	CHECK(sq_cfg_set("GBA_CORE", "gpsp", &e) == 0);
	CHECK(sq_cfg_set("CART_AUTOLOAD", "0", &e) == 0);
	text = read_text("/userdata/retronsq.cfg");
	CHECK(text && strstr(text, "# my settings\r\n") && strstr(text, "GBA_CORE=gpsp\n"));
	CHECK(text && !strstr(text, "vbam") && !strstr(text, "mgba"));	/* duplicates gone */
	CHECK(text && strstr(text, "GBA_CORE_X=1") && strstr(text, "CART_AUTOLOAD=0\n"));
	free(text);
	CHECK(!strcmp(sq_cfg_get("GBA_CORE", v, sizeof(v)), "gpsp"));
	CHECK(sq_cfg_set("GBA_CORE", "x;reboot", &e) != 0);		/* values stay plain */
	CHECK(sq_cfg_set("A=B", "1", &e) != 0);
}

/* The game library: scanning, history, favorites, box art */
static void test_library(void)
{
	struct sq_game* g = NULL;
	char p[SQ_PATH_LEN];
	int n;

	clean();
	put("/userdata/roms/gba/Zelda.gba", "x", 1);
	put("/userdata/roms/gba/sub/advance wars.GBA", "x", 1);
	put("/userdata/roms/gba/Pack.zip", "x", 1);
	put("/userdata/roms/gb/Tetris.gb", "x", 1);
	put("/userdata/roms/gbc/Link.gbc", "x", 1);
	put("/userdata/roms/gba/._Zelda.gba", "x", 1);	/* macOS junk */
	put("/userdata/roms/gba/notes.txt", "x", 1);

	n = sq_library_scan(SQ_ROMS_DIR, "gba", &g);
	CHECK(n == 3);
	CHECK(n == 3 && !strcmp(g[0].label, "advance wars") && !strcmp(g[1].label, "Pack")
		&& !strcmp(g[2].label, "Zelda"));
	CHECK(n == 3 && !strcmp(g[2].path, "/userdata/roms/gba/Zelda.gba"));	/* no $SQ_ROOT */
	CHECK(n == 3 && !strcmp(g[1].system, "gba"));				/* zip by folder */
	free(g);
	n = sq_library_scan(SQ_ROMS_DIR, NULL, &g);
	CHECK(n == 5);
	free(g);
	CHECK(!strcmp(sq_rom_system("/userdata/roms/gbc/x.zip"), "gbc"));

	CHECK(sq_history_add("/userdata/roms/gb/Tetris.gb", "Tetris") == 0);
	CHECK(sq_history_add("/userdata/roms/gba/Zelda.gba", "Zelda") == 0);
	CHECK(sq_history_add("/userdata/roms/gb/Tetris.gb", "Tetris") == 0);
	n = sq_history_load(&g);
	CHECK(n == 2 && !strcmp(g[0].label, "Tetris") && !strcmp(g[1].label, "Zelda"));
	free(g);
	put("/userdata/roms/gba/Zelda.gba", "", 0);
	path(p, "/userdata/roms/gba/Zelda.gba");
	unlink(p);					/* deleted on a PC */
	n = sq_history_load(&g);
	CHECK(n == 1);
	free(g);

	CHECK(sq_favorite_toggle("/userdata/roms/gb/Tetris.gb", "Tetris") == 1);
	CHECK(sq_is_favorite("/userdata/roms/gb/Tetris.gb"));
	CHECK(sq_favorite_toggle("/userdata/roms/gbc/Link.gbc", "Link") == 1);
	n = sq_favorites_load(&g);
	CHECK(n == 2 && !strcmp(g[0].label, "Link"));
	free(g);
	CHECK(sq_favorite_toggle("/userdata/roms/gb/Tetris.gb", "Tetris") == 0);
	CHECK(!sq_is_favorite("/userdata/roms/gb/Tetris.gb"));

	{
		struct sq_game z;
		snprintf(z.label, sizeof(z.label), "Mario & Luigi: Superstar?");
		snprintf(z.system, sizeof(z.system), "gba");
		sq_boxart_path(&z, p, sizeof(p));
		CHECK(strstr(p, "/userdata/retroarch/thumbnails/Nintendo - Game Boy Advance/"
			"Named_Boxarts/Mario _ Luigi_ Superstar_.png") != NULL);

		/* A picture next to the game, with its name, comes first */
		snprintf(z.path, sizeof(z.path), "/userdata/roms/gba/Mario & Luigi.gba");
		put("/userdata/roms/gba/Mario & Luigi.png", "png", 3);
		sq_boxart_path(&z, p, sizeof(p));
		CHECK(strstr(p, "/userdata/roms/gba/Mario & Luigi.png") != NULL);
	}
}

/* The copy meter the three screens share */
static void test_meter(void)
{
	struct sq_meter m;
	char size[64], rate[64];
	const long MB = 1048576;

	sq_meter_start(&m, 16 * MB);
	sq_meter_text(&m, 0, 1000, 0, size, sizeof(size), rate, sizeof(rate));
	CHECK(!strcmp(size, "0.0 of 16.0 MB") && !strcmp(rate, "Measuring speed..."));
	sq_meter_text(&m, 1 * MB, 1000, 0, size, sizeof(size), rate, sizeof(rate));
	sq_meter_text(&m, 2 * MB, 2000, 0, size, sizeof(size), rate, sizeof(rate));
	CHECK(!strcmp(rate, "Measuring speed..."));		/* under 1.5 s of data */
	sq_meter_text(&m, 3 * MB, 3000, 0, size, sizeof(size), rate, sizeof(rate));
	CHECK(!strcmp(size, "3.0 of 16.0 MB") && !strcmp(rate, "1.0 MB/s, about 13 s left"));
	sq_meter_text(&m, 16 * MB, 16000, 1, size, sizeof(size), rate, sizeof(rate));
	CHECK(!strcmp(rate, "Done"));

	sq_meter_start(&m, 32 * MB);			/* a slow cartridge */
	sq_meter_text(&m, 1, 10, 0, size, sizeof(size), rate, sizeof(rate));
	sq_meter_text(&m, 1 + 1024 * 400, 2010, 0, size, sizeof(size), rate, sizeof(rate));
	CHECK(!strncmp(rate, "200 KB/s, about 2 min ", 22));
}

static void test_atomic_write(void)
{
	char p[SQ_PATH_LEN];
	clean();
	put("/f", "old", 3);
	path(p, "/f");
	CHECK(sq_write_atomic(p, "new data", 8, NULL) == 0);
	CHECK(sq_same_content(p, "new data", 8));
	CHECK(!exists("/f.tmp"));
	CHECK(sq_save_size_plausible(131072) && !sq_save_size_plausible(100000));
}

int main(void)
{
	sandbox_init("/tmp/sqcart-test");

	test_crc();
	test_reader_state();
	test_scan_and_cache();
	test_saves();
	test_dump_to_library();
	test_gen3();
	test_atomic_write();
	test_cart_write();
	test_config();
	test_library();
	test_meter();

	printf("libsqcart: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
