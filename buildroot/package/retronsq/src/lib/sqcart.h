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
 * The firmware never writes to the cartridge. It copies the ROM to
 * SQ_ROM_DIR (the "cache"), keeps the live save there as <name>.srm, and
 * backs up cartridge saves in SQ_SAVES_DIR/<name>/NNN-<kind>.sav.
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

/* --- small file helpers ------------------------------------------------- */

int sq_ends_with(const char* name, const char* ext);	/* case-insensitive */
long sq_file_size(const char* path);			/* -1 when missing */
void sq_mkdirs(const char* path);			/* mkdir -p */
/* Read a whole file of at most max bytes; NULL on failure. Free the result. */
unsigned char* sq_read_all(const char* path, long max, long* size);
/* Write via path.tmp + fsync + rename, so a power cut leaves old or new */
int sq_write_atomic(const char* path, const void* data, long size, struct sq_error* e);
/* True when the file holds exactly data */
int sq_same_content(const char* path, const void* data, long size);

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

/* Progress callback: total bytes done so far */
typedef void (*sq_progress_fn)(long done, void* ctx);

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
 * Copy the cartridge's ROM into the cache, checked against its .crc. A bad
 * read is retried once; if it still doesn't match (common with repros) the
 * copy is kept so the game runs, but without a cached .crc, so it is read
 * again next time. *verified tells which. Returns 0 when a copy exists.
 */
int sq_cache_cartridge(const struct sq_cart* c, int* verified,
	sq_progress_fn progress, void* ctx, struct sq_error* e);

/*
 * Copy the cartridge into the library: SQ_ROMS_DIR/<sysdir>/<name>, with its
 * save as <name>.srm next to it. A game already there (same CRC) isn't
 * copied again (*already set); a different game with the same name becomes
 * "<name> (2)". out receives the library path. Returns 0 on success.
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
/* Back up the cartridge's own save (if it has one); same return */
int sq_backup_cart_save(const struct sq_cart* c, int* existed, struct sq_error* e);
/* List a game's backups, newest first; returns how many (at most max) */
int sq_backup_list(const char* game, struct sq_backup* out, int max);
/* Make the file at path the cartridge game's live save, backing up the
 * current one first. Returns 0 on success. */
int sq_restore_save(const struct sq_cart* c, const char* path, struct sq_error* e);

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

#ifdef __cplusplus
}
#endif

#endif
