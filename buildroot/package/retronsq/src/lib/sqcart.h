/*
 * libsqcart: everything the Retron SQ firmware knows about cartridges, in one
 * place. Used by sqload (the loading screen), sqtools (Cartridge Tools), the
 * sqcart command (for the shell scripts) and, later, the home UI.
 *
 * The cartridge slot is a USB reader. With a cartridge in, its drive is
 * mounted read-only at SQ_READER_DIR (by cart-event) and holds:
 *   <name>.gb / .gbc / .gba   the ROM
 *   <name>.sav                the cartridge's save memory, if any
 *   <name>.crc                4-byte CRC of the ROM (big endian, see crc.c)
 *
 * The firmware doesn't play from the cartridge or save to it. It copies the
 * ROM to SQ_ROM_DIR (the "cache"), keeps the live save there as
 * <name>.srm, and backs up cartridge saves in
 * SQ_SAVES_DIR/<name>/NNN-<kind>.sav. The one exception is Cartridge
 * Tools' "Write a save/game to the cartridge", asked for explicitly
 * (sq_write_cart_save, sq_write_cart_rom).
 *
 * All paths go through sq_path(), which prefixes $SQ_ROOT when it is set, so
 * tests can run against a sandbox. Functions report failures in a struct
 * sq_error (what failed + errno) for messages and logs.
 */
#ifndef SQCART_H
#define SQCART_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- paths (on the console; see sq_path) -------------------------------- */

#define SQ_READER_DIR	"/media/cart"		/* the cartridge, read-only */
#define SQ_ROM_DIR	"/userdata/rom"		/* cartridge copies + live saves */
#define SQ_ROMS_DIR	"/userdata/roms"	/* the user's game library */
#define SQ_SAVES_DIR	"/userdata/saves"	/* save backups, a folder per game */
#define SQ_RUN_DIR	"/run/retronsq"		/* runtime state (tmpfs) */
#define SQ_READER_PORT	"/sys/bus/usb/devices/3-1"	/* the reader's USB port */
#define SQ_PATH_LEN	1024

/* Write path (with $SQ_ROOT in front when set) into out; returns out */
char* sq_path(char* out, size_t len, const char* path);

/* --- errors and logging ------------------------------------------------- */

struct sq_error {
	char msg[160];	/* "reading the cartridge: Input/output error" */
	int err;	/* errno, or 0 */
};

/* Record a failure: what was being done, plus errno's text when err != 0 */
void sq_fail(struct sq_error* e, int err, const char* fmt, ...);

/* Log a line to stderr (and the console log, through the caller) */
void sq_log(const char* fmt, ...);
/* Also log a line to logs/launcher.log on the SD card, synced at once
 * (like LogSD in common.sh) */
void sq_log_sd(const char* fmt, ...);

/* --- copy progress as the screens show it ------------------------------ */

/*
 * One meter per copy, shared by the loading screen, the home screen and
 * Cartridge Tools so they all say the same: "8.0 of 16.0 MB" and
 * "1.0 MB/s, about 8 s left". The speed is the average since the first
 * data, shown once 1.5 s of it has arrived (before that it jumps around).
 */
struct sq_meter {
	long total;
	unsigned long first_ms;	/* when data first came (0: not yet) */
	long first_bytes;
};
void sq_meter_start(struct sq_meter* m, long total);
/* size: "8.0 of 16.0 MB" ("64 of 128 KB" under 1 MB); rate: "1.0 MB/s, about 8 s left", "Measuring
 * speed..." or "Done". ASCII only (the tools core's font has nothing else). */
void sq_meter_text(struct sq_meter* m, long done, unsigned long now_ms, int finished,
	char* size, size_t size_len, char* rate, size_t rate_len);

/* Progress callback: bytes done so far. Each action says what it counts
 * up to (its total), so the screen can show a meter. */
typedef void (*sq_progress_fn)(long done, void* ctx);

/* --- small file helpers ------------------------------------------------- */

int sq_ends_with(const char* name, const char* ext);	/* case-insensitive */
long sq_file_size(const char* path);			/* -1 when missing */
void sq_mkdirs(const char* path);			/* mkdir -p */
/* Read a whole file of at most max bytes; NULL on failure. Free the result. */
unsigned char* sq_read_all(const char* path, long max, long* size);
/* Same, reporting base + bytes read so far to progress (may be NULL) */
unsigned char* sq_read_all_progress(const char* path, long max, long* size,
	sq_progress_fn progress, void* ctx, long base);
/* Write via path.tmp + fsync + rename + fsync of the folder, so a power
 * cut leaves the old file or the new one. The folder's fsync matters: on
 * the FAT card the rename otherwise sits in memory for up to 30 s, and the
 * console is switched off at the power. */
int sq_write_atomic(const char* path, const void* data, long size, struct sq_error* e);
/* Flush the folder holding path (its renames and new files) to the card */
void sq_sync_dir(const char* path);
/* True when the file holds exactly data */
int sq_same_content(const char* path, const void* data, long size);

/* --- the clock ------------------------------------------------------------ */

/*
 * The console has no battery-backed clock: every boot starts in 1970, and
 * files (saves) got dates like that. So the time is saved on the SD card
 * (SQ_HOME_DIR/clock) every few minutes and before a restart or power off,
 * and at boot the clock is set to that, or to when the firmware was built
 * if that is later. Set once on the home screen (Settings > Date and
 * time), it then keeps roughly right, only losing the time it was off.
 */
int sq_clock_save(void);
/* The time to set at boot, or 0 when the clock is already later */
long sq_clock_wanted(void);
/* Set the clock (needs root) and save it. 0 on success. */
int sq_clock_set(long t, struct sq_error* e);

/* --- the reader's CRC (crc.c) ------------------------------------------- */

#define SQ_CRC_BLOCK 512
/* Feed len bytes; blocks are 512 bytes except a file's last one */
uint32_t sq_crc_update(uint32_t crc, const uint8_t* data, size_t len);
/* CRC of a whole buffer, in the reader's 512-byte blocks */
uint32_t sq_crc_buffer(uint32_t crc, const uint8_t* data, size_t len);
/* Read a 4-byte big-endian .crc file; 0 on success */
int sq_crc_read_file(const char* path, uint32_t* crc);

/* --- the reader --------------------------------------------------------- */

enum sq_reader_state {
	SQ_READER_NONE,		/* no reader device on the USB port */
	SQ_READER_PENDING,	/* reader there, cartridge not mounted yet */
	SQ_READER_EMPTY,	/* reader there, no medium (no cartridge) */
	SQ_READER_READY,	/* cartridge mounted at SQ_READER_DIR */
};
enum sq_reader_state sq_reader_state(void);
const char* sq_reader_state_name(enum sq_reader_state s);
/* Bumped by cart-event whenever the cartridge changes (inserted, removed,
 * swapped); a copy checks it didn't change underneath it */
unsigned sq_reader_generation(void);

/* --- the cartridge ------------------------------------------------------ */

struct sq_cart {
	int present;
	char rom[256];		/* file name on the reader, "Pokemon_Emerald.gba" */
	char base[256];		/* without its extension */
	const char* system;	/* "Game Boy Advance", "Game Boy Color", "Game Boy" */
	const char* sysdir;	/* its folder under roms/: "gba", "gbc", "gb" */
	long rom_size;
	long save_size;		/* 0 when the cartridge has no save */
	char title[17];		/* from the ROM header, e.g. "POKEMON LEAF" */
	char code[5];		/* GBA game code, e.g. "BPGE"; empty for GB */
	unsigned generation;	/* sq_reader_generation() when scanned */
};

/* Look at the reader and fill c (c->present = 0 when there's no cartridge) */
void sq_cart_scan(struct sq_cart* c);
/* True while the same cartridge is still mounted (not removed or swapped) */
int sq_cart_still_there(const struct sq_cart* c);
/* Full path of a file on the reader / in the cache for this cartridge */
void sq_cart_reader_path(const struct sq_cart* c, const char* ext, char* out, size_t len);
void sq_cart_cache_path(const struct sq_cart* c, const char* ext, char* out, size_t len);

/* Title and GBA game code from a ROM file's header */
void sq_rom_header(const char* path, char title[17], char code[5]);

/* True when SQ_ROM_DIR already holds a verified copy of this cartridge */
int sq_cache_valid(const struct sq_cart* c);

/*
 * Copy src to dst through dst.part (renamed only when complete), reading and
 * writing in parallel threads with large buffers. When crc is not NULL the
 * reader's CRC of the data is stored there. Returns 0 on success.
 */
int sq_copy(const char* src, const char* dst, uint32_t* crc,
	sq_progress_fn progress, void* ctx, struct sq_error* e);
/* The reader's CRC of a file, without copying it */
int sq_file_crc(const char* path, uint32_t* crc, sq_progress_fn progress,
	void* ctx, struct sq_error* e);

/*
 * How far a copy of the cartridge can be trusted (*verified below):
 *   SQ_VERIFIED_CRC    its checksum matches the one the slot gives (.crc)
 *   SQ_VERIFIED_TWICE  it doesn't, but two reads gave identical data. Dirty
 *                      contacts make reads differ; a mismatch that repeats
 *                      exactly is the slot's checksum disagreeing (it does
 *                      for some genuine cartridges), not a bad read.
 *   SQ_UNVERIFIED      neither (kept so the game runs, read again next time)
 * Mismatches are logged to launcher.log with both checksums.
 */
enum { SQ_UNVERIFIED, SQ_VERIFIED_CRC, SQ_VERIFIED_TWICE };

/*
 * Copy the cartridge's ROM into the cache, checked against its .crc. A
 * mismatch is read again (see SQ_VERIFIED_TWICE); an unverified copy is
 * kept so the game runs, but without a cached .crc, so it is read again
 * next time. Returns 0 when a copy exists.
 */
int sq_cache_cartridge(const struct sq_cart* c, int* verified,
	sq_progress_fn progress, void* ctx, struct sq_error* e);

/*
 * Copy the cartridge into the library: SQ_ROMS_DIR/<sysdir>/<name>, with its
 * save as <name>.srm next to it. A game already there (same data) isn't
 * copied again (*already set); a different game with the same name becomes
 * "<name> (2)". *verified as for sq_cache_cartridge. out receives the
 * library path. Returns 0 on success.
 */
int sq_dump_to_library(const struct sq_cart* c, char* out, size_t len,
	int* already, int* verified, int* save_copied,
	sq_progress_fn progress, void* ctx, struct sq_error* e);

/* --- saves -------------------------------------------------------------- */

/* Largest save we handle (GBA flash is 128 KB) */
#define SQ_MAX_SAVE (4 * 1024 * 1024)

/*
 * First play of a cartridge: copy its save to the cache as <name>.srm, the
 * live save RetroArch uses. Never touches an existing .srm. Returns 0 when
 * there is a live save afterwards (or the cartridge has none).
 */
int sq_import_save(const struct sq_cart* c, struct sq_error* e);

/*
 * Each game's folder in SQ_SAVES_DIR holds a copy of the cartridge's own
 * save, cartridge.sav, refreshed whenever the cartridge is played, and
 * backups (NNN-<kind>.sav) made when asked for or before something is
 * overwritten.
 */
#define SQ_CART_SAVE_COPY "cartridge.sav"
void sq_cart_save_copy_path(const char* game, char* out, size_t len);
/* Make cartridge.sav match the cartridge's save (*changed when it was
 * written), and drop backups identical to an older backup. 0 on success
 * (or when the cartridge has no save). */
int sq_sync_cart_save(const struct sq_cart* c, int* changed, struct sq_error* e);

/* A backup in SQ_SAVES_DIR/<game>/ */
struct sq_backup {
	int number;		/* NNN */
	char kind[32];		/* "cartridge", "before-restore" */
	char path[SQ_PATH_LEN];
	long size;
};

/* Store data as backup NNN-<kind>.sav unless an identical one exists
 * (then *existed = 1). Returns the backup's number, or -1. */
int sq_backup_store(const char* game, const void* data, long size,
	const char* kind, int* existed, struct sq_error* e);
/* Back up the cartridge's own save (if it has one); same return. Reading
 * it is reported to progress as base + bytes read (up to c->save_size). */
int sq_backup_cart_save(const struct sq_cart* c, int* existed,
	sq_progress_fn progress, void* ctx, long base, struct sq_error* e);
/* The same as NNN-<kind>.sav ("before-write"...) */
int sq_backup_cart_save_as(const struct sq_cart* c, const char* kind, int* existed,
	sq_progress_fn progress, void* ctx, long base, struct sq_error* e);
/* List a game's backups, newest first; returns how many (at most max) */
int sq_backup_list(const char* game, struct sq_backup* out, int max);
/* Make the file at path the cartridge game's live save, backing up the
 * current one first. Reading it is reported to progress (up to its size).
 * Returns 0 on success. */
int sq_restore_save(const struct sq_cart* c, const char* path,
	sq_progress_fn progress, void* ctx, struct sq_error* e);

/* --- writing to the cartridge -------------------------------------------- */

/*
 * Only for "Write a save to the cartridge" and "Write a game to the
 * cartridge", which the user asks for and confirms. The cartridge is
 * mounted read-only; the caller opens it for writing (cart-event
 * write-enable) just before and closes it again (write-disable) right
 * after, and backs up the cartridge's save first (see sq_do_write_*).
 */

/* Whether the reader accepts writes, as its drive reported when it was
 * mounted (cart-event records it): 1 yes, 0 write-protected, -1 unknown.
 * Only "0" means much: a slot that doesn't report write protection may
 * still not pass writes on to the cartridge. */
int sq_reader_writable(void);

/*
 * What writing to this cartridge (same ROM file name and size) did when it
 * was tried: 1 it worked (confirmed when the cartridge was put back), 0 it
 * didn't stick, -1 not tried yet. game: 1 for writing a game, 0 a save.
 * Recorded by the write check and by a write that read back different.
 */
int sq_cart_write_known(const struct sq_cart* c, int game);
void sq_cart_write_record(const struct sq_cart* c, int game, int ok);

/*
 * Write data over the cartridge's save file, in place: same size, no
 * truncation, no new file, so the drive's FAT and directory don't change
 * (the reader maps the file's sectors to the save chip). Then read it back
 * past the page cache and compare. Refuses a save of another size than the
 * cartridge's. Progress: base + bytes written, then read back (2 x size).
 * Returns 0 when written and read back identical.
 */
int sq_write_cart_save(const struct sq_cart* c, const void* data, long size,
	sq_progress_fn progress, void* ctx, long base, struct sq_error* e);

/*
 * A game for the cartridge: only for a rewritable (flash) cartridge, which
 * an original one isn't, and only if the slot passes the writes on. What
 * can be checked beforehand: a .gba for a Game Boy Advance cartridge, a
 * .gb/.gbc for a Game Boy one, no bigger than the cartridge's game (the
 * file on the drive can't grow). 0 when it fits, else why not in e.
 */
int sq_cart_rom_fits(const struct sq_cart* c, const char* path, struct sq_error* e);
/* Write the game at path over the start of the cartridge's game file, in
 * place like a save, then read it back and compare. Progress: base + bytes
 * written, then read back (2 x the game's size). 0 when identical. */
int sq_write_cart_rom(const struct sq_cart* c, const char* path,
	sq_progress_fn progress, void* ctx, long base, struct sq_error* e);

/*
 * Did it really reach the cartridge? A slot can take a write, and even read
 * it back, without storing it on the cartridge (keeping it in its own
 * memory, say). The only proof is the cartridge being taken out and put
 * back. So after a write, what was written is remembered
 * (sq_write_check_remember, in SQ_HOME_DIR, so a power-off in between is
 * fine), and the next time that cartridge is inserted sq_write_check
 * compares its save and fills r with the verdict. Returns 1 when it checked
 * (the record is then dropped), 0 when nothing was pending for this
 * cartridge.
 */
struct sq_result;	/* see "cartridge actions" below */
void sq_write_check_remember(const struct sq_cart* c, const void* data, long size);
/* The same for a game written to it (the start of the game is compared,
 * whatever name the cartridge comes back under) */
void sq_write_check_remember_rom(const struct sq_cart* c, const char* path);
int sq_write_check(const struct sq_cart* c, struct sq_result* r);

/* --- save checks -------------------------------------------------------- */

/* Expected save sizes, and whether size is one a cartridge can have */
int sq_save_size_plausible(long size);

/* Pokémon Ruby/Sapphire/Emerald/FireRed/LeafGreen (game code prefixes
 * AXV, AXP, BPE, BPR, BPG) */
int sq_is_gen3_pokemon(const char* code);

struct sq_gen3_slot {
	int valid;		/* all 14 sections present, signed, checksums ok */
	unsigned counter;	/* save counter (higher = newer) */
	int sections;		/* sections found (0..14) */
	int bad_checksums;
	int bad_signatures;
};
struct sq_gen3_report {
	long size;
	int truncated;		/* smaller than the 128 KB flash */
	struct sq_gen3_slot slot[2];
	int newest;		/* 0 or 1 = slot the game loads, -1 none valid */
};
/* Check a Gen 3 Pokémon save the way the game does */
void sq_gen3_check(const uint8_t* data, long size, struct sq_gen3_report* r);

/* --- settings (retronsq.cfg) ------------------------------------------- */

#define SQ_CFG_FILE	"/userdata/retronsq.cfg"		/* the user's settings */
#define SQ_DEFAULTS_FILE "/usr/share/retronsq/defaults.cfg"	/* every default */

/*
 * A setting's value, read like the scripts' CfgGet: KEY=value lines, spaces
 * around "=", quotes and Windows line endings are fine, the last line for
 * a key wins, and a missing or empty value falls back to defaults.cfg.
 * Writes "" when there is neither. Returns out.
 */
char* sq_cfg_get(const char* key, char* out, size_t len);
/* Set a setting: its first line is replaced (later duplicates removed), or
 * a line is added; the rest of the file, comments included, is kept.
 * Written atomically. Returns 0 on success. */
int sq_cfg_set(const char* key, const char* value, struct sq_error* e);

/* --- the game library (home screen) -------------------------------------- */

#define SQ_HOME_DIR	"/userdata/home"	/* history.txt, favorites.txt */
#define SQ_THUMBS_DIR	"/userdata/retroarch/thumbnails"	/* RetroArch's */
#define SQ_HISTORY_MAX	30

struct sq_game {
	char path[SQ_PATH_LEN];	/* on the console (no $SQ_ROOT): /userdata/... */
	char label[128];	/* what is shown: the file name without extension */
	char system[4];		/* "gba", "gbc" or "gb" */
};

/* The system a ROM file is for, from its extension (any case); a .zip goes
 * by the gba/gbc/gb folder it is in. "gba", "gbc" or "gb". */
const char* sq_rom_system(const char* path);
/* RetroArch's playlist name for a system ("Nintendo - Game Boy Advance") */
const char* sq_system_name(const char* system);
/* Games under dir (e.g. SQ_ROMS_DIR, recursively; hidden files skipped),
 * for one system or all (system NULL), sorted by label ignoring case.
 * *out is malloc'ed (free it); returns how many, or -1. */
int sq_library_scan(const char* dir, const char* system, struct sq_game** out);
/* Recently played (newest first) and favorites, from SQ_HOME_DIR. Entries
 * whose file is gone are left out. Same return as sq_library_scan. */
int sq_history_load(struct sq_game** out);
int sq_favorites_load(struct sq_game** out);
/* Put a game at the top of the history (at most SQ_HISTORY_MAX kept) */
int sq_history_add(const char* path, const char* label);
/* Add or remove a favorite; returns 1 when it is a favorite afterwards,
 * 0 when not, -1 on error */
int sq_favorite_toggle(const char* path, const char* label);
int sq_is_favorite(const char* path);
/* A game's box art (full path, with $SQ_ROOT): a PNG next to the game with
 * its name (roms/gba/Golden Sun.png) when there is one, else RetroArch's:
 * SQ_THUMBS_DIR/<system name>/Named_Boxarts/<label>.png, with &*\/:`<>?|
 * in the label replaced by _ */
char* sq_boxart_path(const struct sq_game* g, char* out, size_t len);

/* A save on the SD card: a game's live save (<game>.srm next to it in
 * roms/, or a cartridge's in SQ_ROM_DIR) or a backup in SQ_SAVES_DIR */
struct sq_save_file {
	char path[SQ_PATH_LEN];	/* on the console: /userdata/... */
	char label[128];	/* the game */
	char kind[48];		/* "Game Boy Advance save", "Save for playing the
				 * cartridge", "Copy of the cartridge's save",
				 * "Backup #2 (cartridge)" */
	long size;
	long mtime;
	int live;		/* the save a game plays with */
	int copy;		/* saves/<game>/cartridge.sav */
	int backup;		/* a backup's number, else 0 */
};
/* Every save, by game (ignoring case): the live save, the copy of the
 * cartridge's save, then backups newest first. *out is malloc'ed; returns
 * how many. */
int sq_saves_scan(struct sq_save_file** out);

/* Delete a game: a .gba/.gbc/.gb/.zip in SQ_ROMS_DIR, or a cartridge copy in
 * SQ_ROM_DIR (with its .crc). Its saves are kept. Anything else is
 * refused. path as on the console. 0 on success. */
int sq_delete_game(const char* path, struct sq_error* e);
/* Delete a save: a .srm in SQ_ROMS_DIR or SQ_ROM_DIR, or a backup .sav in
 * SQ_SAVES_DIR. Anything else is refused. 0 on success. */
int sq_delete_save(const char* path, struct sq_error* e);

/* --- cartridge actions (Cartridge Tools and the home screen) ------------- */

/* What an action did, for the screen: a few short lines (the first one is
 * the headline) and whether it worked */
#define SQ_RESULT_COLS 50
struct sq_result {
	int ok;
	char line[4][SQ_RESULT_COLS + 1];
};

/* A save that can be restored or written to the cartridge */
struct sq_save_choice {
	char label[64];
	char path[SQ_PATH_LEN];
};
/*
 * The saves to offer, newest first: for restoring, the save on the
 * cartridge now, then the backups; for writing to the cartridge, the save
 * played with now (the live save), then the backups. Returns how many.
 */
int sq_save_choices(const struct sq_cart* c, int for_writing,
	struct sq_save_choice* out, int max);

/*
 * The actions report progress so the screens can show a meter; each counts
 * up to (its total):
 *   copy to library   c->rom_size
 *   backup save       c->save_size
 *   restore save      the size of the save at path
 *   write save        3 x c->save_size (backup, write, read back)
 *   write game        c->save_size + 2 x the game's size
 */

/* Copy cartridge to SD card (roms/<system>/, with its save). *added tells
 * whether a new game landed in the library (playlists need a refresh). */
void sq_do_copy_to_library(const struct sq_cart* c, sq_progress_fn progress,
	void* ctx, int* added, struct sq_result* r);
/* Back up the cartridge's save into saves/<game>/ */
void sq_do_backup_save(const struct sq_cart* c, sq_progress_fn progress, void* ctx,
	struct sq_result* r);
/* Make the save at path the one played with (the current one is backed up) */
void sq_do_restore_save(const struct sq_cart* c, const char* path,
	sq_progress_fn progress, void* ctx, struct sq_result* r);
/*
 * Experimental: write the save at path onto the cartridge. Checks first
 * (reader write protection, size, Gen 3 sanity), backs up the cartridge's
 * save, then runs "<rw_cmd> write-enable", sq_write_cart_save and
 * "<rw_cmd> write-disable". Every step is logged with sq_log_sd.
 */
void sq_do_write_save(const struct sq_cart* c, const char* path, const char* rw_cmd,
	sq_progress_fn progress, void* ctx, struct sq_result* r);
/* Experimental: write the game at path onto a rewritable cartridge, the
 * same way (sq_cart_rom_fits, save backup, write-enable, sq_write_cart_rom,
 * write-disable) */
void sq_do_write_rom(const struct sq_cart* c, const char* path, const char* rw_cmd,
	sq_progress_fn progress, void* ctx, struct sq_result* r);

#ifdef __cplusplus
}
#endif

#endif
