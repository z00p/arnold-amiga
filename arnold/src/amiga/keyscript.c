/* Key scripts for test runs (same text format as AmiMSX's tools/keyscript.py):
 *     <time> <KEY>[+KEY...] [hold] [every <period> [until <time>]]
 *     <time> SNAP                dump the frame (PNG) into the snap directory
 *     <time> QUIT                leave the emulation (the run is over)
 * Times and lengths are CPC frames (50 Hz) or seconds with an 's' suffix,
 * counted from power on; hold defaults to 5 frames. '#' starts a comment.
 * Keys: CPC names (SPACE LEFT RIGHT UP DOWN RETURN ENTER ESC TAB DEL CLR SHIFT
 * CTRL COPY CAPS F0-F9 FDOT, 0-9, A-Z, - ^ @ [ ] ; : , . / \) and the joystick
 * (JOYUP JOYDOWN JOYLEFT JOYRIGHT FIRE1 FIRE2). Plain C, no OS calls. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../cpc/cpc.h"
#include "keyscript.h"

#define MAXEV 8192
typedef struct { long frame; short key; char down; char kind; } Ev;   /* kind: 0 key, 1 snap, 2 quit */
static Ev ev[MAXEV];
static int nev, next;

static const struct { const char *name; int key; } names[] = {
	{"SPACE", CPC_KEY_SPACE}, {"LEFT", CPC_KEY_CURSOR_LEFT}, {"RIGHT", CPC_KEY_CURSOR_RIGHT},
	{"UP", CPC_KEY_CURSOR_UP}, {"DOWN", CPC_KEY_CURSOR_DOWN}, {"RETURN", CPC_KEY_RETURN},
	{"ENTER", CPC_KEY_SMALL_ENTER}, {"ESC", CPC_KEY_ESC}, {"TAB", CPC_KEY_TAB}, {"DEL", CPC_KEY_DEL},
	{"CLR", CPC_KEY_CLR}, {"SHIFT", CPC_KEY_SHIFT}, {"CTRL", CPC_KEY_CONTROL}, {"COPY", CPC_KEY_COPY},
	{"CAPS", CPC_KEY_CAPS_LOCK}, {"F0", CPC_KEY_F0}, {"F1", CPC_KEY_F1}, {"F2", CPC_KEY_F2},
	{"F3", CPC_KEY_F3}, {"F4", CPC_KEY_F4}, {"F5", CPC_KEY_F5}, {"F6", CPC_KEY_F6}, {"F7", CPC_KEY_F7},
	{"F8", CPC_KEY_F8}, {"F9", CPC_KEY_F9}, {"FDOT", CPC_KEY_FDOT},
	{"JOYUP", CPC_KEY_JOY_UP}, {"JOYDOWN", CPC_KEY_JOY_DOWN}, {"JOYLEFT", CPC_KEY_JOY_LEFT},
	{"JOYRIGHT", CPC_KEY_JOY_RIGHT}, {"FIRE1", CPC_KEY_JOY_FIRE1}, {"FIRE2", CPC_KEY_JOY_FIRE2},
	{"-", CPC_KEY_MINUS}, {"^", CPC_KEY_HAT}, {"@", CPC_KEY_AT}, {"[", CPC_KEY_OPEN_SQUARE_BRACKET},
	{"]", CPC_KEY_CLOSE_SQUARE_BRACKET}, {";", CPC_KEY_SEMICOLON}, {":", CPC_KEY_COLON},
	{",", CPC_KEY_COMMA}, {".", CPC_KEY_DOT}, {"/", CPC_KEY_FORWARD_SLASH}, {"\\", CPC_KEY_BACKSLASH},
	{NULL, 0} };
static const int digits[10] = { CPC_KEY_ZERO, CPC_KEY_1, CPC_KEY_2, CPC_KEY_3, CPC_KEY_4,
	CPC_KEY_5, CPC_KEY_6, CPC_KEY_7, CPC_KEY_8, CPC_KEY_9 };
static const int letters[26] = { CPC_KEY_A, CPC_KEY_B, CPC_KEY_C, CPC_KEY_D, CPC_KEY_E, CPC_KEY_F,
	CPC_KEY_G, CPC_KEY_H, CPC_KEY_I, CPC_KEY_J, CPC_KEY_K, CPC_KEY_L, CPC_KEY_M, CPC_KEY_N, CPC_KEY_O,
	CPC_KEY_P, CPC_KEY_Q, CPC_KEY_R, CPC_KEY_S, CPC_KEY_T, CPC_KEY_U, CPC_KEY_V, CPC_KEY_W, CPC_KEY_X,
	CPC_KEY_Y, CPC_KEY_Z };

static int keyid(const char *s)
{
	int i;
	if (strlen(s) == 1) {
		if (isdigit((unsigned char)s[0])) return digits[s[0] - '0'];
		if (isalpha((unsigned char)s[0])) return letters[toupper((unsigned char)s[0]) - 'A'];
	}
	for (i = 0; names[i].name; i++) if (!strcmp(names[i].name, s)) return names[i].key;
	return -1;
}

static long timeval(const char *s)      /* "30s", "5.2s" or "1500" -> frames; no strtod (libnix) */
{
	long whole = 0, frac = 0, scale = 1;
	while (isdigit((unsigned char)*s)) whole = whole * 10 + (*s++ - '0');
	if (*s == '.') { s++; while (isdigit((unsigned char)*s) && scale < 1000) { frac = frac * 10 + (*s++ - '0'); scale *= 10; } }
	if (*s == 's') return whole * 50 + (frac * 50 + scale / 2) / scale;
	return whole;
}

static void add(long frame, int key, int down, int kind)
{
	if (nev >= MAXEV) return;
	ev[nev].frame = frame; ev[nev].key = key; ev[nev].down = down; ev[nev].kind = kind; nev++;
}

static int cmp(const void *a, const void *b)
{
	long d = ((const Ev *)a)->frame - ((const Ev *)b)->frame;
	return d < 0 ? -1 : d > 0 ? 1 : (((const Ev *)a)->down - ((const Ev *)b)->down);  /* releases first */
}

int KeyScript_Load(const char *name)
{
	FILE *f = fopen(name, "r");
	char line[256], *tok[12];
	int ntok, i;
	nev = next = 0;
	if (!f) return -1;
	while (fgets(line, sizeof line, f)) {
		char *p = strchr(line, '#'); if (p) *p = 0;
		ntok = 0;
		for (p = strtok(line, " \t\r\n"); p && ntok < 12; p = strtok(NULL, " \t\r\n")) tok[ntok++] = p;
		if (ntok < 2) continue;
		{
			long t = timeval(tok[0]), hold = 5, every = 0, until = -1;
			char *keys = tok[1];
			if (!strcmp(keys, "SNAP")) { add(t, 0, 0, 1); continue; }
			if (!strcmp(keys, "QUIT")) { add(t, 0, 0, 2); continue; }
			for (i = 2; i < ntok; i++) {
				if (!strcmp(tok[i], "every") && i + 1 < ntok) { every = timeval(tok[++i]); }
				else if (!strcmp(tok[i], "until") && i + 1 < ntok) { until = timeval(tok[++i]); }
				else hold = timeval(tok[i]);
			}
			if (every <= 0) { every = 1; until = t; }
			if (until < 0) until = t;
			for (; t <= until; t += every) {
				char buf[64], *k;
				strncpy(buf, keys, sizeof buf - 1); buf[sizeof buf - 1] = 0;
				for (k = strtok(buf, "+"); k; k = strtok(NULL, "+")) {
					int id = keyid(k);
					if (id < 0) continue;
					add(t, id, 1, 0);
					add(t + hold, id, 0, 0);
				}
			}
		}
	}
	fclose(f);
	qsort(ev, nev, sizeof(Ev), cmp);
	return nev;
}

/* Apply everything due at this frame. Returns a KS_ flag set. */
int KeyScript_Frame(long frame, void (*setkey)(int key, int down))
{
	int flags = 0;
	while (next < nev && ev[next].frame <= frame) {
		if (ev[next].kind == 0) setkey(ev[next].key, ev[next].down);
		else if (ev[next].kind == 1) flags |= KS_SNAP;
		else flags |= KS_QUIT;
		next++;
	}
	return flags;
}

int KeyScript_Active(void) { return nev > 0 && next < nev; }

int KeyScript_SnapAt(long frame)
{
	int i;
	for (i = next; i < nev && ev[i].frame <= frame; i++)
		if (ev[i].frame == frame && ev[i].kind == 1) return 1;
	return 0;
}
