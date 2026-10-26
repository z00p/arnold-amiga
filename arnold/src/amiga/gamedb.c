/* Game settings database: CRC-32 of the image and the Games.db parser (see gamedb.h). */
#include <stdio.h>
#include <string.h>
#include "gamedb.h"

unsigned long GameDB_Crc32Update(unsigned long crc, const unsigned char *p, unsigned long n)
{
	static unsigned long tab[256];
	static int made;
	unsigned long i, k;
	if (!made) {
		for (i = 0; i < 256; i++) {
			unsigned long c = i;
			for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
			tab[i] = c;
		}
		made = 1;
	}
	crc = ~crc & 0xFFFFFFFFUL;
	while (n--) crc = tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return ~crc & 0xFFFFFFFFUL;
}

int GameDB_CrcFile(const char *path, unsigned long *crc)
{
	unsigned char buf[4096];
	unsigned long c = 0;
	size_t n;
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0) c = GameDB_Crc32Update(c, buf, (unsigned long)n);
	if (ferror(f)) { fclose(f); return 0; }
	fclose(f);
	*crc = c;
	return 1;
}

/* one line into line[GDB_LINE]; a longer line is cut and its rest skipped, so it can
 * never spill into the next line. 0 at the end of the file. */
static int read_line(FILE *f, char *line)
{
	int n = 0, c;
	for (;;) {
		c = fgetc(f);
		if (c == EOF) { line[n] = 0; return n > 0; }
		if (c == '\n' || c == '\r') {
			if (n == 0 && c == '\r') continue;   /* part of CR+LF, or an empty line: next */
			break;
		}
		if (n < GDB_LINE - 1) line[n++] = (char)c;
	}
	line[n] = 0;
	return 1;
}

/* "[1A2B3C4D] Title": 8 hex digits exactly; the CRC in *crc, the title (after the
 * bracket, spaces skipped) in *title. 0 if the line is not a header. */
static int parse_header(const char *s, unsigned long *crc, const char **title)
{
	unsigned long v = 0;
	int i;
	if (*s != '[') return 0;
	s++;
	for (i = 0; i < 8; i++, s++) {
		int d;
		if (*s >= '0' && *s <= '9') d = *s - '0';
		else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
		else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
		else return 0;
		v = (v << 4) | (unsigned long)d;
	}
	if (*s != ']') return 0;
	s++;
	while (*s == ' ' || *s == '\t') s++;
	*crc = v; *title = s;
	return 1;
}

int GameDB_Lookup(FILE *db, unsigned long crc, GameDB_SetFn set, void *user, char *title, int tsize)
{
	char line[GDB_LINE], *v, *k, *e;
	unsigned long hc;
	const char *t;
	int matched = 0, in_block = 0, settings = 0;

	if (title && tsize > 0) title[0] = 0;
	if (!db) return 0;
	crc &= 0xFFFFFFFFUL;
	while (read_line(db, line)) {
		k = line;
		while (*k == ' ' || *k == '	') k++;
		if (!*k || *k == ';' || *k == '#') continue;
		if (*k == '[') {
			if (!parse_header(k, &hc, &t)) continue;   /* a header we can't read: skipped */
			if (settings) {                            /* a new game: the last block is over */
				if (matched) break;
				in_block = 0; settings = 0;
			}
			if (hc == crc) {
				in_block = 1;
				if (!matched && title && tsize > 0) { strncpy(title, t, (size_t)tsize - 1); title[tsize - 1] = 0; }
				matched = 1;
			}
			continue;
		}
		v = strchr(k, '=');
		if (!v) continue;
		*v++ = 0;
		e = k + strlen(k);                             /* trim the key's trailing blanks, the value's leading ones */
		while (e > k && (e[-1] == ' ' || e[-1] == '	')) *--e = 0;
		while (*v == ' ' || *v == '	') v++;
		settings = 1;
		if (in_block && set) set(k, v, user);
	}
	return matched;
}

unsigned long GameDB_LastCrc;
int GameDB_LastCrcOk;

int GameDB_Settings(const char *prefs_name, const char *disk_a, const char *dbpath,
	GameDB_SetFn set, void *user, char *title, int tsize)
{
	char img[1024], line[GDB_LINE], *v;
	unsigned long crc;
	size_t n = strlen(prefs_name);
	FILE *f;
	int hit;

	if (title && tsize > 0) title[0] = 0;
	GameDB_LastCrcOk = 0;
	f = fopen(prefs_name, "r");
	if (f) {
		while (read_line(f, line)) {
			v = strchr(line, '=');
			if (!v) continue;
			*v++ = 0;
			if (set) set(line, v, user);
		}
		fclose(f);
		return GDB_PREFS;
	}
	if (n < 7 || n - 6 > sizeof img - 8) return GDB_NONE;
	memcpy(img, prefs_name, n - 6); img[n - 6] = 0;   /* the image: the .prefs name without ".prefs" */
	if (disk_a && disk_a[0] && !strcmp(img, disk_a)) {
		strcat(img, ".bak");
		f = fopen(img, "rb");
		if (f) fclose(f); else img[n - 6] = 0;
	}
	if (!GameDB_CrcFile(img, &crc)) return GDB_NONE;
	GameDB_LastCrc = crc; GameDB_LastCrcOk = 1;
	f = fopen(dbpath, "r");
	if (!f) return GDB_NONE;
	hit = GameDB_Lookup(f, crc, set, user, title, tsize);
	fclose(f);
	return hit ? GDB_DB : GDB_NONE;
}
