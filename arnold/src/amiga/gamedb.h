#ifndef GAMEDB_H
#define GAMEDB_H
/* Game settings database (Games.db, 1.2). Plain C, stdio only, no OS calls, so
 * the parser is unit-tested on the PC (tools/gamedb_test.sh).
 *
 * File format (text, CR or LF line ends, lines up to GDB_LINE-1 characters):
 *   ; comment            (also '#'), blank lines ignored
 *   [A1B2C3D4] Title     header: the CRC-32 (8 hex digits) of a cartridge, disk A or
 *                        tape image, then the title as a comment. Several headers in
 *                        a row share the settings lines that follow them.
 *   key=value            the settings of a game's .prefs file (sound, hires, joykeys,
 *                        tall, wide, remap, diskwrite, fasttape, tapesound, autotape), applied
 *                        to the headers just above them. Unknown keys are passed on
 *                        too; the caller ignores what it does not know. */
#include <stdio.h>

#define GDB_LINE 192            /* longest line read (a remap= line is at most 6 + 127) */

unsigned long GameDB_Crc32Update(unsigned long crc, const unsigned char *p, unsigned long n);   /* start with 0 */
int GameDB_CrcFile(const char *path, unsigned long *crc);     /* 1 = read, 0 = can't open / read error */
/* Reads db up to the block with this CRC and calls set(key, value, user) for each of
 * its settings lines. title (may be NULL) gets the header's title. Returns 1 if the
 * CRC has a block, 0 if not. Never writes outside the given buffers. */
typedef void (*GameDB_SetFn)(const char *key, const char *val, void *user);
int GameDB_Lookup(FILE *db, unsigned long crc, GameDB_SetFn set, void *user, char *title, int tsize);

/* The settings a game starts with. prefs_name is the game's own settings file
 * ("<image>.prefs"): if it opens, its lines go to set() and GDB_PREFS is returned
 * (Save for game always wins). Otherwise the image it names is hashed, <image>.bak
 * instead when disk_a is that image and the .bak exists (a disk's write-back
 * renames the original to .bak the first time and never replaces it, so the hash
 * of a disk that has been saved to is still that of the original), and the block
 * of that CRC in the database file dbpath goes to set(): GDB_DB. GDB_NONE if there
 * is neither. title (may be NULL) gets the database title on GDB_DB. */
#define GDB_NONE 0
#define GDB_PREFS 1
#define GDB_DB 2
extern unsigned long GameDB_LastCrc;   /* the CRC GameDB_Settings hashed last, valid when ... */
extern int GameDB_LastCrcOk;           /* ... this is 1 (0: none computed: a .prefs file was used, or the image can't be read) */
int GameDB_Settings(const char *prefs_name, const char *disk_a, const char *dbpath,
	GameDB_SetFn set, void *user, char *title, int tsize);
#endif
