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

static void le32w(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* Write a valid slot: 14 sectors, rotated like the game does */
static void make_slot(uint8_t* save, int slot, uint32_t counter, unsigned seed)
{
	int i;
	srand(seed);
	for (i = 0; i < 14; i++) {
		uint8_t* sec = save + (slot * 14 + i) * 0x1000;
		int id = (i + counter) % 14, j;
		uint32_t sum = 0;
		int size = id == 0 ? 3884 : id == 4 ? 3848 : id == 13 ? 2000 : 3968;
		memset(sec, 0, 0x1000);
		for (j = 0; j < size; j++)
			sec[j] = rand() & 0xFF;
		for (j = 0; j < 3968; j += 4)
			sum += sec[j] | sec[j + 1] << 8 | sec[j + 2] << 16 | (uint32_t)sec[j + 3] << 24;
		sec[0xFF4] = id;
		sec[0xFF6] = (uint16_t)((sum >> 16) + sum) & 0xFF;
		sec[0xFF7] = (uint16_t)((sum >> 16) + sum) >> 8;
		le32w(sec + 0xFF8, 0x08012025u);
		le32w(sec + 0xFFC, counter);
	}
}

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

	printf("libsqcart: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
