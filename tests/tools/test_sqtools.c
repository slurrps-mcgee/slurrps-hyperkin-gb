/*
 * Cartridge Tools (the sqtools libretro core) driven like RetroArch drives
 * it: one retro_run() per frame, buttons through the input callback. The
 * core's source is included so the test can look at its screen and
 * messages. Runs against a sandbox (../lib/sandbox.h) with a fake reader.
 *
 * Built with SQ_TEST_READ_DELAY_US so copies take long enough to pull the
 * cartridge midway. SQTOOLS_SHOTS=DIR saves each screen as DIR/<name>.ppm.
 * Built and run by tests/run-all.sh.
 */
#define QUIT_RETROARCH()	(quit_requested = 1)
#define SCAN_CMD		"true"
/* cart-event write-enable/-disable: nothing to do in the sandbox (its
 * files are writable); fails while $SQ_TEST_RW_FAIL is set */
#define CART_RW_CMD		"test -z \"$SQ_TEST_RW_FAIL\" || false"
static int quit_requested, shutdown_requested;

#include "sandbox.h"
#include "sqtools.c"

static int16_t pad[16];

static bool env(unsigned cmd, void* data)
{
	(void)data;
	if (cmd == RETRO_ENVIRONMENT_SHUTDOWN) {
		shutdown_requested = 1;
		return true;
	}
	return cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT || cmd == RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME;
}
static void video(const void* d, unsigned w, unsigned h, size_t pitch) { (void)d; (void)w; (void)h; (void)pitch; }
static size_t audio(const int16_t* d, size_t f) { (void)d; return f; }
static void poll_input(void) {}
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
	(void)port; (void)dev; (void)idx;
	return id < 16 ? pad[id] : 0;
}

static void frames(int n)
{
	while (n--)
		retro_run();
}

static void press(unsigned id)
{
	pad[id] = 1;
	frames(1);
	pad[id] = 0;
	frames(1);
}

/* Let a task finish (at most 20 s), drawing frames meanwhile */
static void wait_task(void)
{
	int i;
	for (i = 0; i < 10000 && screen == SCREEN_BUSY; i++) {
		frames(1);
		usleep(2000);
	}
}

/* Move the cursor to ITEM on the main screen and press A */
static void choose(int item)
{
	int i;
	for (i = 0; i < ITEM_COUNT && main_sel != item; i++)
		press(RETRO_DEVICE_ID_JOYPAD_DOWN);
	press(RETRO_DEVICE_ID_JOYPAD_A);
}

/* On a confirm screen: move to Yes and press it (No is the default) */
static void yes(void)
{
	press(RETRO_DEVICE_ID_JOYPAD_RIGHT);
	press(RETRO_DEVICE_ID_JOYPAD_A);
}

/* Back to the main screen from a message */
static void dismiss(void)
{
	press(RETRO_DEVICE_ID_JOYPAD_A);
}

static void shot(const char* name)
{
	const char* dir = getenv("SQTOOLS_SHOTS");
	char p[SQ_PATH_LEN];
	FILE* fp;
	int i;
	if (!dir)
		return;
	snprintf(p, sizeof(p), "%s/%s.ppm", dir, name);
	fp = fopen(p, "wb");
	if (!fp)
		return;
	fprintf(fp, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
	for (i = 0; i < WIDTH * HEIGHT; i++) {
		unsigned char rgb[3] = { (fb[i] >> 11) << 3, ((fb[i] >> 5) & 63) << 2, (fb[i] & 31) << 3 };
		fwrite(rgb, 1, 3, fp);
	}
	fclose(fp);
}

static int message_is(int line, const char* text)
{
	if (screen == SCREEN_MESSAGE && !strcmp(msg[line], text))
		return 1;
	fprintf(stderr, "  screen %d, message line %d is '%s', expected '%s'\n",
		screen, line, msg[line], text);
	return 0;
}

static int same_file(const char* a_rel, const char* b_rel)
{
	char a[SQ_PATH_LEN], b[SQ_PATH_LEN];
	long size = 0;
	unsigned char* data;
	int same;
	path(a, a_rel);
	path(b, b_rel);
	data = sq_read_all(a, 64 * 1024 * 1024, &size);
	same = data && sq_same_content(b, data, size);
	free(data);
	return same;
}

int main(void)
{
	sandbox_init("/tmp/sqtools-test");
	/* SQ_TEST_VERSION: the label for documentation screenshots */
	put_text("/etc/retronsq-version", "Retron SQ CFW %s\n",
		getenv("SQ_TEST_VERSION") ? getenv("SQ_TEST_VERSION") : "v0.2.2 beta");

	retro_set_environment(env);
	retro_set_video_refresh(video);
	retro_set_audio_sample_batch(audio);
	retro_set_input_poll(poll_input);
	retro_set_input_state(input);
	retro_init();
	retro_load_game(NULL);
	frames(2);

	/* Header shows the firmware version */
	CHECK(!strcmp(fw_version, getenv("SQ_TEST_VERSION") ? getenv("SQ_TEST_VERSION") : "v0.2.2 beta"));

	/* No cartridge: only Exit works */
	shot("no-cartridge");
	CHECK(!cart.present);
	CHECK(!ItemEnabled(ITEM_PLAY) && !ItemEnabled(ITEM_DUMP_ROM) && !ItemEnabled(ITEM_RESTORE));
	press(RETRO_DEVICE_ID_JOYPAD_A);
	CHECK(!quit_requested && screen == SCREEN_MAIN);

	/* Inserted while the tools are open: noticed within half a second */
	insert_cart("Pokemon_LeafGreen.gba", 2 * 1024 * 1024, 128 * 1024, 1, 1, 1);
	frames(31);
	CHECK(cart.present && !strcmp(cart.rom, "Pokemon_LeafGreen.gba"));
	shot("main");

	/* Copy cartridge to SD card: ROM and save into roms/gba/ */
	choose(ITEM_DUMP_ROM);
	CHECK(screen == SCREEN_BUSY);
	shot("copying");
	wait_task();
	shot("copied");
	CHECK(message_is(0, "Copied to") && message_is(1, "roms/gba/"));
	CHECK(message_is(3, "Its save was copied too."));
	CHECK(same_file("/media/cart/Pokemon_LeafGreen.gba", "/userdata/roms/gba/Pokemon_LeafGreen.gba"));
	CHECK(same_file("/media/cart/Pokemon_LeafGreen.sav", "/userdata/roms/gba/Pokemon_LeafGreen.srm"));
	dismiss();

	/* Again: recognised, not copied twice */
	choose(ITEM_DUMP_ROM);
	wait_task();
	CHECK(message_is(0, "This cartridge is already on the SD card:"));
	dismiss();

	/* Back up cartridge save, twice */
	choose(ITEM_DUMP_SAVE);
	wait_task();
	CHECK(message_is(0, "The cartridge's save is backed up in"));
	CHECK(message_is(1, "saves/Pokemon_LeafGreen/ as #1"));
	CHECK(exists("/userdata/saves/Pokemon_LeafGreen/001-cartridge.sav"));
	dismiss();
	choose(ITEM_DUMP_SAVE);
	wait_task();
	CHECK(message_is(0, "This save is already backed up:"));
	dismiss();

	/* Restore a save: the live save becomes backup #1; the one replaced is
	 * kept as a "before restore" backup */
	put("/userdata/rom/Pokemon_LeafGreen.srm", "live save", 9);
	choose(ITEM_RESTORE);
	CHECK(screen == SCREEN_RESTORE && backup_count == 2);
	CHECK(!strcmp(backups[0].label, "Save on the cartridge now (128 KB)"));
	shot("restore-list");
	press(RETRO_DEVICE_ID_JOYPAD_DOWN);
	press(RETRO_DEVICE_ID_JOYPAD_A);
	CHECK(screen == SCREEN_CONFIRM && confirm_button == 0);
	shot("restore-confirm");
	press(RETRO_DEVICE_ID_JOYPAD_A);		/* "No" is selected: cancels */
	CHECK(screen == SCREEN_RESTORE);
	press(RETRO_DEVICE_ID_JOYPAD_A);
	yes();
	wait_task();
	CHECK(message_is(0, "Save restored."));
	CHECK(same_file("/userdata/saves/Pokemon_LeafGreen/001-cartridge.sav", "/userdata/rom/Pokemon_LeafGreen.srm"));
	CHECK(size_of("/userdata/saves/Pokemon_LeafGreen/002-before-restore.sav") == 9);
	dismiss();

	/* The "Copy failed" bug: the cartridge is swapped while the tools are
	 * open and Copy is chosen before the tools noticed. The tools must say
	 * what happened, then copy the new cartridge. */
	main_sel = 0;
	remove_cart();
	insert_cart("Golden_Sun.gba", 1024 * 1024, 0, 7, 1, 2);
	choose(ITEM_DUMP_ROM);
	shot("swapped");
	CHECK(message_is(0, "The cartridge was removed or changed."));
	dismiss();
	CHECK(!strcmp(cart.rom, "Golden_Sun.gba"));
	CHECK(!ItemEnabled(ITEM_DUMP_SAVE));		/* this one has no save */
	choose(ITEM_DUMP_ROM);
	wait_task();
	CHECK(message_is(0, "Copied to"));
	CHECK(same_file("/media/cart/Golden_Sun.gba", "/userdata/roms/gba/Golden_Sun.gba"));
	dismiss();

	/* Pulled in the middle of a copy: a clear error, no half file. (Here
	 * the reads still succeed, as the sandbox's files stay readable once
	 * opened; a cartridge whose .crc doesn't match can't vouch for them.) */
	main_sel = 0;
	remove_cart();
	insert_cart("Pokemon_Emerald.gba", 4 * 1024 * 1024, 0, 9, 0, 3);
	frames(31);
	choose(ITEM_DUMP_ROM);
	frames(3);
	remove_cart();
	put_text("/run/retronsq/cart-generation", "4\n");
	wait_task();
	shot("pulled");
	CHECK(message_is(0, "Copy failed:"));
	CHECK(message_is(1, "the cartridge was removed while it was being read"));
	CHECK(!exists("/userdata/roms/gba/Pokemon_Emerald.gba"));
	CHECK(!exists("/userdata/roms/gba/Pokemon_Emerald.gba.part"));
	dismiss();
	CHECK(!cart.present);

	/* Write a save to the cartridge (experimental). Pokemon LeafGreen
	 * (code BPGE), 128 KB save. */
	insert_cart("Pokemon_LeafGreen.gba", 2 * 1024 * 1024, 128 * 1024, 1, 1, 6);
	frames(31);
	CHECK(ItemEnabled(ITEM_WRITE_SAVE));
	{
		unsigned char* good = calloc(1, 131072);
		unsigned char* before;
		char sav[SQ_PATH_LEN];
		long size;

		make_slot(good, 0, 7, 3);
		make_slot(good, 1, 8, 4);
		put("/userdata/rom/Pokemon_LeafGreen.srm", good, 131072);
		path(sav, "/media/cart/Pokemon_LeafGreen.sav");
		before = sq_read_all(sav, SQ_MAX_SAVE, &size);

		/* The list starts with the live save; asks twice */
		main_sel = 0;
		choose(ITEM_WRITE_SAVE);
		CHECK(screen == SCREEN_RESTORE && writing);
		CHECK(!strcmp(backups[0].label, "The save you play with now (128 KB)"));
		shot("write-list");
		press(RETRO_DEVICE_ID_JOYPAD_A);
		CHECK(screen == SCREEN_CONFIRM && confirm_step == 0 && confirm_button == 0);
		yes();
		CHECK(screen == SCREEN_CONFIRM && confirm_step == 1 && confirm_button == 0);
		shot("write-confirm");
		press(RETRO_DEVICE_ID_JOYPAD_B);
		CHECK(screen == SCREEN_RESTORE);

		/* A write-protected reader: refused, nothing changed */
		put_text("/run/retronsq/reader-wp", "1\n");
		press(RETRO_DEVICE_ID_JOYPAD_A);
		yes();
		yes();
		wait_task();
		CHECK(message_is(0, "This cartridge slot can't write:"));
		CHECK(sq_same_content(sav, before, size));
		dismiss();
		put_text("/run/retronsq/reader-wp", "0\n");

		/* The cartridge can't be opened for writing: nothing changed */
		setenv("SQ_TEST_RW_FAIL", "1", 1);
		main_sel = 0;
		choose(ITEM_WRITE_SAVE);
		press(RETRO_DEVICE_ID_JOYPAD_A);
		yes();
		yes();
		wait_task();
		CHECK(message_is(0, "The cartridge couldn't be opened for writing."));
		CHECK(sq_same_content(sav, before, size));
		dismiss();
		unsetenv("SQ_TEST_RW_FAIL");

		/* Written: the cartridge holds the live save, its old save is
		 * backed up, and the log says so */
		main_sel = 0;
		choose(ITEM_WRITE_SAVE);
		press(RETRO_DEVICE_ID_JOYPAD_A);
		yes();
		yes();
		wait_task();
		shot("written");
		CHECK(message_is(0, "Written to the cartridge."));
		CHECK(exists("/userdata/home/cart-write-check"));	/* checked at re-insert */
		CHECK(sq_same_content(sav, good, 131072));
		{
			/* (an identical backup from earlier counts: no duplicates) */
			struct sq_backup list[8];
			int n = sq_backup_list("Pokemon_LeafGreen", list, 8), i, found = 0;
			for (i = 0; i < n; i++)
				found |= !strcmp(list[i].kind, "cartridge") &&
					sq_same_content(list[i].path, before, size);
			CHECK(found);
		}
		CHECK(size_of("/userdata/logs/launcher.log") > 0);
		dismiss();

		/* A damaged Pokemon save or one of another size: refused */
		good[14 * 0x1000 + 5] ^= 0xFF;
		good[5] ^= 0xFF;
		put("/userdata/rom/Pokemon_LeafGreen.srm", good, 131072);
		main_sel = 0;
		choose(ITEM_WRITE_SAVE);
		press(RETRO_DEVICE_ID_JOYPAD_A);
		yes();
		yes();
		wait_task();
		CHECK(message_is(0, "That Pokemon save is damaged"));
		dismiss();
		put("/userdata/rom/Pokemon_LeafGreen.srm", good, 65536);
		main_sel = 0;
		choose(ITEM_WRITE_SAVE);
		press(RETRO_DEVICE_ID_JOYPAD_A);
		yes();
		yes();
		wait_task();
		CHECK(message_is(0, "That save doesn't fit this cartridge."));
		dismiss();
		free(before);
		free(good);
	}
	remove_cart();

	/* Play cartridge: asks sq-session for it and leaves RetroArch */
	insert_cart("Pokemon_LeafGreen.gba", 2 * 1024 * 1024, 128 * 1024, 1, 1, 5);
	put_text("/run/retronsq/cart-failed", "");
	frames(31);
	main_sel = ITEM_EXIT;
	choose(ITEM_PLAY);
	CHECK(quit_requested);
	CHECK(exists("/run/retronsq/play-cart") && !exists("/run/retronsq/cart-failed"));

	/* Exit */
	screen = SCREEN_MAIN;
	choose(ITEM_EXIT);
	CHECK(shutdown_requested);

	retro_unload_game();
	retro_deinit();
	printf("sqtools: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
