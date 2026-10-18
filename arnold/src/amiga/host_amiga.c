/* Arnold host for AmigaOS: implements src/cpc/host.h on top of amiga_sys.c.
 * The core renders into an 8-bit chunky buffer (384 x 272, one byte per
 * pixel, PaletteRemap indices); amiga_sys converts it to AGA bitplanes.
 * Keyboard: Amiga rawkeys -> CPC key matrix, or the joystick (JOYKEYS, see joykey()). Joystick port 2 -> CPC joystick 0.
 * Sound: Paula, 8-bit stereo, 22.2 kHz, one block a frame (NOSOUND turns it off). */
#include "../cpc/host.h"
#include "../cpc/arnold.h"
#include "../cpc/cpc.h"
#include "../cpc/render.h"
#include "../cpc/asic.h"
#include "../cpc/snapshot.h"
#include "../cpc/diskimage/diskimg.h"
#include "amiga_sys.h"
#include "screenshot.h"
#include "keyscript.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

unsigned char *chunky;          /* W x H, one byte a pixel: 384 x 272 lores, 768 x 272 hires */
int W, H;
extern void Palette_Set(void);

static GRAPHICS_BUFFER_INFO bi;
static GRAPHICS_BUFFER_COLOUR_FORMAT cf;
/* 444 samples a CPC frame (the core makes rate / 50.08), played at Paula
 * period 160: 444 x 160 = 71040 colour clocks, one PAL frame (71051). */
#define AUD_PERIOD 160
#define AUD_SAMPLES 444
static SOUND_PLAYBACK_FORMAT sf = {2, 16, 22240};
static struct SysMenu menu;
static int display_open, audio_open;
unsigned char hostpal[256][3];
static FILE *keylog;                  /* KEYLOG argument: raw key codes to PROGDIR:keys.log */
/* Test runs: KEYS=<script> drives the CPC keyboard by frame, SNAPDIR=<dir> takes
 * the SNAP dumps (<frame>.png) and a "done" file at QUIT. */
static int script_on;
static int turbo;                     /* TURBO argument: no 50 Hz wait, screen every 25th frame */
static unsigned long turbo_count;
static long script_frame;
static char snapdir[SYS_PATH] = "PROGDIR:snaps";
static int script_quit;
/* Frame skip (14 Oct 2026): when the emulation ends a frame behind the 50 Hz
 * schedule, the next frame is emulated but not drawn, so a slow machine keeps
 * the game and its sound at full speed and shows fewer frames. skip_max is the
 * most frames skipped in a row (SKIP=n, ToolType SKIP=n, prefs skip=n; 0 = off).
 * REFRESH=n (ToolType, prefs refresh=n) draws at most one frame in n, steadily,
 * like AmiMSX's Refresh setting; the adaptive skip works on top of it.
 * A frame a test script will SNAP is always drawn. The core asks about the
 * frame after next (it turns drawing on early, see arnold.c). */
static int skip_max = 3, skip_run, refresh = 1;
static unsigned long refresh_count;
static long behind_us;
extern int (*CPC_SkipNextFrameHook)(void);

/* Performance figures for a scripted run, from frame PERF_SKIP (after the
 * cartridge has started) to QUIT. Times come from the Amiga's E-clock, so in
 * WinUAE they are emulated-machine time. "work" = emulation core + c2p (with
 * its chip RAM writes): what has to fit into the 20 ms of a PAL frame. */
#define PERF_SKIP 50
static unsigned long perf_t0, perf_t1, perf_snap_us;

static void perf_report(FILE *f)
{
	long n = script_frame - 1 - PERF_SKIP;            /* frames measured */
	unsigned long total, c2p, wait, core, work, cf;
	if (n <= 0 || !perf_t0) return;
	total = perf_t1 - perf_t0;
	c2p = Sys_Perf.c2p_us; wait = Sys_Perf.wait_us;
	core = total - c2p - wait - Sys_Perf.pal_us - perf_snap_us;
	work = (core + c2p + Sys_Perf.pal_us) / n;         /* us of work per CPC frame */
	cf = Sys_Perf.c2p_frames ? Sys_Perf.c2p_frames : 1;
	fprintf(f, "perf frames=%ld wall_ms=%lu fps_x10=%lu\n", n, total / 1000, (unsigned long)n * 10000UL / (total / 1000 ? total / 1000 : 1));   /* 32-bit safe */
	fprintf(f, "perf core_us_per_frame=%lu c2p_us_per_shown_frame=%lu wait_us_per_frame=%lu\n",
		core / n, c2p / cf, wait / n);
	fprintf(f, "perf c2p_lines_per_shown_frame=%lu chip_bytes_per_shown_frame=%lu shown_frames=%lu\n",
		Sys_Perf.c2p_lines / cf, Sys_Perf.c2p_lines * Sys_Perf.line_bytes / cf, Sys_Perf.c2p_frames);
	fprintf(f, "perf work_us_per_frame=%lu budget_percent=%lu\n", work, work * 100 / 20000);
	fprintf(f, "perf wait_safe_us_per_frame=%lu wait_disp_us_per_frame=%lu wait_throttle_us_per_frame=%lu throttle_calls=%lu\n",
		Sys_Perf.wait_safe_us / n, Sys_Perf.wait_disp_us / n, Sys_Perf.wait_throttle_us / n, Sys_Perf.throttle_calls);
	fprintf(f, "perf late_frames=%lu palette_us_per_frame=%lu palette_loads=%lu\n", Sys_Perf.late_frames, Sys_Perf.pal_us / n, Sys_Perf.palette_loads);
	fprintf(f, "perf skipped_frames=%lu skip_max=%d refresh=%d shown_fps_x10=%lu\n", Sys_Perf.skipped_frames, skip_max, refresh, Sys_Perf.c2p_frames * 10000UL / (total / 1000 ? total / 1000 : 1));
	fprintf(f, "perf sound=%d audio_underruns=%lu audio_drops=%lu\n", audio_open, Sys_Perf.aud_underruns, Sys_Perf.aud_drops);
}

static void script_setkey(int key, int down) { if (down) CPC_SetKey(key); else CPC_ClearKey(key); }

static void script_snap(void)
{
	char name[SYS_PATH + 32];
	extern unsigned char *chunky; extern int W, H; extern unsigned char hostpal[256][3];
	sprintf(name, "%s/%05ld.png", snapdir, script_frame);
	Screenshot_SavePNG(name, chunky, W, H, W, hostpal);
}
static unsigned char audio_tmp[4096];
static unsigned long audio_tmp_len;

/* Amiga rawkey -> CPC key. UK Amiga layout; the CPC keys @ [ : ; ] sit where
 * the Amiga has [ ] ; ' #. -1 = no CPC key. Help is a host key. */
static const signed char rawmap[128] = {
/*00*/ CPC_KEY_HAT, CPC_KEY_1, CPC_KEY_2, CPC_KEY_3, CPC_KEY_4, CPC_KEY_5, CPC_KEY_6, CPC_KEY_7,
/*08*/ CPC_KEY_8, CPC_KEY_9, CPC_KEY_ZERO, CPC_KEY_MINUS, CPC_KEY_HAT, CPC_KEY_BACKSLASH, -1, CPC_KEY_F0,
/*10*/ CPC_KEY_Q, CPC_KEY_W, CPC_KEY_E, CPC_KEY_R, CPC_KEY_T, CPC_KEY_Y, CPC_KEY_U, CPC_KEY_I,
/*18*/ CPC_KEY_O, CPC_KEY_P, CPC_KEY_AT, CPC_KEY_OPEN_SQUARE_BRACKET, -1, CPC_KEY_F1, CPC_KEY_F2, CPC_KEY_F3,
/*20*/ CPC_KEY_A, CPC_KEY_S, CPC_KEY_D, CPC_KEY_F, CPC_KEY_G, CPC_KEY_H, CPC_KEY_J, CPC_KEY_K,
/*28*/ CPC_KEY_L, CPC_KEY_COLON, CPC_KEY_SEMICOLON, CPC_KEY_CLOSE_SQUARE_BRACKET, -1, CPC_KEY_F4, CPC_KEY_F5, CPC_KEY_F6,
/*30*/ -1, CPC_KEY_Z, CPC_KEY_X, CPC_KEY_C, CPC_KEY_V, CPC_KEY_B, CPC_KEY_N, CPC_KEY_M,
/*38*/ CPC_KEY_COMMA, CPC_KEY_DOT, CPC_KEY_FORWARD_SLASH, -1, CPC_KEY_FDOT, CPC_KEY_F7, CPC_KEY_F8, CPC_KEY_F9,
/*40*/ CPC_KEY_SPACE, CPC_KEY_DEL, CPC_KEY_TAB, CPC_KEY_SMALL_ENTER, CPC_KEY_RETURN, CPC_KEY_ESC, CPC_KEY_CLR, -1,
/*48*/ -1, -1, -1, -1, CPC_KEY_CURSOR_UP, CPC_KEY_CURSOR_DOWN, CPC_KEY_CURSOR_RIGHT, CPC_KEY_CURSOR_LEFT,
/*50*/ CPC_KEY_F1, CPC_KEY_F2, CPC_KEY_F3, CPC_KEY_F4, CPC_KEY_F5, CPC_KEY_F6, CPC_KEY_F7, CPC_KEY_F8,
/*58*/ CPC_KEY_F9, CPC_KEY_F0, -1, -1, -1, -1, -1, -1,
/*60*/ CPC_KEY_SHIFT, CPC_KEY_SHIFT, CPC_KEY_CAPS_LOCK, CPC_KEY_CONTROL, CPC_KEY_COPY, -1, -1, -1,   /* Right Alt: joystick fire 2, see joykey() */
/*68*/ -1, -1, -1, -1, -1, -1, -1, -1,
/*70*/ -1, -1, -1, -1, -1, -1, -1, -1,
/*78*/ -1, -1, -1, -1, -1, -1, -1, -1 };
#define RAW_HELP 0x5f

void Host_HandlePrinterOutput(void) {}

BOOL Host_SetDisplay(int Type, int Width, int Height, int Depth)
{
	if (!display_open) {
		W = Width; H = Height;
		chunky = malloc(W * H);
		if (!chunky) return FALSE;
		memset(chunky, 0, W * H);
		if (!Sys_OpenDisplay(W, H, W > 400)) return FALSE;
		display_open = 1;
	}
	return TRUE;
}

GRAPHICS_BUFFER_COLOUR_FORMAT *Host_GetGraphicsBufferColourFormat(void)
{
	cf.BPP = 8;
	cf.Red.BPP = 3; cf.Red.Mask = 0xe0; cf.Red.Shift = 5;
	cf.Green.BPP = 3; cf.Green.Mask = 0x1c; cf.Green.Shift = 2;
	cf.Blue.BPP = 2; cf.Blue.Mask = 0x03; cf.Blue.Shift = 0;
	return &cf;
}

GRAPHICS_BUFFER_INFO *Host_GetGraphicsBufferInfo(void)
{
	bi.pSurface = chunky; bi.Width = W; bi.Height = H; bi.Pitch = W;
	return &bi;
}

BOOL Host_LockGraphicsBuffer(void) { return TRUE; }
void Host_UnlockGraphicsBuffer(void) {}

void Host_SwapGraphicsBuffers(void)
{
	/* Palette_Set() is commented out in render5.c's Render_DumpDisplay, so
	 * push the pens the core allocated this frame ourselves. */
	Palette_Set();
	/* TURBO (test runs): the core still renders every frame into chunky (SNAP
	 * dumps it), but only every 25th frame goes to the screen. */
	Sys_OsdSkip = skip_max;
	if (!turbo || (turbo_count++ % 25) == 0) Sys_ShowFrame(chunky, W);
}

void Host_SetPaletteEntry(int i, unsigned char r, unsigned char g, unsigned char b)
{
	if (i >= 0 && i < 256) { hostpal[i][0] = r; hostpal[i][1] = g; hostpal[i][2] = b; }
	Sys_SetPaletteEntry(i, r, g, b);
}

/* Screenshot: PROGDIR:Arnold-NNN.iff and .png, first free number. */
static void screenshot(void)
{
	char iff[64], png[64];
	int n;
	FILE *t;
	if (!chunky) return;
	for (n = 1; n < 1000; n++) {
		sprintf(iff, "PROGDIR:Arnold-%03d.iff", n);
		t = fopen(iff, "rb"); if (!t) break; fclose(t);
	}
	sprintf(png, "PROGDIR:Arnold-%03d.png", n);
	if (Screenshot_SaveILBM(iff, chunky, W, H, W, hostpal, W > 400) && Screenshot_SavePNG(png, chunky, W, H, W, hostpal))
		sprintf(menu.status, "Saved Arnold-%03d.iff and .png", n);
	else
		sprintf(menu.status, "Screenshot failed (Arnold-%03d)", n);
}

/* ---- sound: Paula via amiga_sys, one block a frame ---- */
void Host_WriteDataToSoundBuffer(unsigned char *p, unsigned long l) {}
BOOL Host_AudioPlaybackPossible(void) { return audio_open; }
SOUND_PLAYBACK_FORMAT *Host_GetSoundPlaybackFormat(void) { return &sf; }
BOOL Host_LockAudioBuffer(unsigned char **p1, unsigned long *l1, unsigned char **p2, unsigned long *l2, int size)
{
	if (!audio_open || size > (int)sizeof(audio_tmp)) return FALSE;
	*p1 = audio_tmp; *l1 = size; *p2 = NULL; *l2 = 0;
	audio_tmp_len = size;
	return TRUE;
}
void Host_UnLockAudioBuffer(void) { Sys_AudioQueue(audio_tmp, audio_tmp_len / 4); }

/* Once a frame: the core mixes the frame's sound (Host_LockAudioBuffer /
 * Host_UnLockAudioBuffer queue it for Paula), then the 50 Hz wait. The Windows
 * host does the same; without the CPC_UpdateAudio call there is no sound. */
void Host_Throttle(void)
{
	if (audio_open) CPC_UpdateAudio();
	behind_us = turbo ? 0 : Sys_WaitFrame();   /* TURBO: run flat out */
}

static int skip_hook(void)
{
	int fixed = refresh > 1 && (refresh_count++ % refresh) != 0;
	if (script_on && KeyScript_SnapAt(script_frame + 1)) { skip_run = 0; return 0; }
	if (fixed) { Sys_Perf.skipped_frames++; return 1; }
	if (skip_max <= 0 || behind_us <= 0 || skip_run >= skip_max) { skip_run = 0; return 0; }
	skip_run++;
	Sys_Perf.skipped_frames++;
	return 1;
}
void Host_DoDriveLEDIndicator(int Drive, BOOL State) {}
void Host_SetDirectory(char *d) {}
unsigned long Host_GetCurrentTimeInMilliseconds(void) { return Sys_Millis(); }

static unsigned joy_prev;

/* Keyboard as joystick (16 Oct 2026). joykeys: 0 = off (the keyboard is the CPC
 * keyboard; Space also gives fire 2 and Right Alt is fire 2), 1 = cursor keys
 * move, Z / Ctrl fire 1, X / Space fire 2, 2 = QAOP move, Space fire 1, M fire 2.
 * In modes 1 and 2 those keys drive only the joystick. The keyboard's joystick
 * bits (kb_joy) are ORed with the real joystick every frame, so the pad and the
 * keys can be used together and releasing one never cancels the other; any
 * number of keys may be held at once. */
static int joykeys, global_joykeys;
static unsigned kb_joy;
static const struct { unsigned char mode, code; unsigned bit; } joykey_map[] = {
	{1, 0x4c, SYS_JOY_UP}, {1, 0x4d, SYS_JOY_DOWN}, {1, 0x4f, SYS_JOY_LEFT}, {1, 0x4e, SYS_JOY_RIGHT},
	{1, 0x31, SYS_JOY_FIRE1}, {1, 0x63, SYS_JOY_FIRE1}, {1, 0x32, SYS_JOY_FIRE2}, {1, 0x40, SYS_JOY_FIRE2},
	{2, 0x10, SYS_JOY_UP}, {2, 0x20, SYS_JOY_DOWN}, {2, 0x18, SYS_JOY_LEFT}, {2, 0x19, SYS_JOY_RIGHT},
	{2, 0x40, SYS_JOY_FIRE1}, {2, 0x37, SYS_JOY_FIRE2},
	{0, 0x40, SYS_JOY_FIRE2}, {1, 0x65, SYS_JOY_FIRE2}, {2, 0x65, SYS_JOY_FIRE2}, {0, 0x65, SYS_JOY_FIRE2},
};
/* returns 1 if the key belongs to the joystick only (no CPC key) */
static int joykey(int code, int down)
{
	int i, only = 0, hit = 0;
	for (i = 0; i < (int)(sizeof joykey_map / sizeof joykey_map[0]); i++)
		if (joykey_map[i].code == code && joykey_map[i].mode == joykeys) {
			if (down) kb_joy |= joykey_map[i].bit; else kb_joy &= ~joykey_map[i].bit;
			hit = 1;
		}
	if (hit && (joykeys != 0 || code == 0x65)) only = 1;   /* off: Space stays the CPC Space too */
	return only;
}

BOOL Host_ProcessSystemEvents(void)
{
	int code, down, qual;
	unsigned joy, diff;
	static const struct { unsigned bit; int key; } jk[] = {
		{SYS_JOY_UP, CPC_KEY_JOY_UP}, {SYS_JOY_DOWN, CPC_KEY_JOY_DOWN},
		{SYS_JOY_LEFT, CPC_KEY_JOY_LEFT}, {SYS_JOY_RIGHT, CPC_KEY_JOY_RIGHT},
		{SYS_JOY_FIRE1, CPC_KEY_JOY_FIRE1}, {SYS_JOY_FIRE2, CPC_KEY_JOY_FIRE2} };
	int i;

	if (script_on) {
		int fl = KeyScript_Frame(script_frame, script_setkey);
		if (script_frame == PERF_SKIP) { perf_t0 = Sys_Micros(); memset(&Sys_Perf, 0, sizeof Sys_Perf); perf_snap_us = 0; }
		if (fl & KS_SNAP) { unsigned long t = Sys_Micros(); script_snap(); perf_snap_us += Sys_Micros() - t; }
		if (fl & KS_QUIT) { script_quit = 1; perf_t1 = Sys_Micros(); Sys_RequestQuit(); }
		script_frame++;
	}
	{ static long hb; if (++hb % 50 == 0) Sys_Heartbeat(hb, behind_us); }
	if (Sys_Poll()) return TRUE;
	while (Sys_GetKey(&code, &down, &qual)) {
		if (keylog) { fprintf(keylog, "%02x %s q=%04x\n", code, down ? "down" : "up", qual); fflush(keylog); }
		if (code == RAW_HELP) { if (down) Sys_RequestQuit(); continue; }
		if (code < 128 && joykey(code, down)) continue;
		if (code == 0x0d) {
			/* the Amiga's \ key: plain = the CPC's \, with Shift = the CPC's |
			 * (Shift+@), which RSX commands such as |CPM and Orion Prime's loader
			 * need (17 Oct 2026). Both cleared on release. */
			if (down) CPC_SetKey((qual & 3) ? CPC_KEY_AT : CPC_KEY_BACKSLASH);
			else { CPC_ClearKey(CPC_KEY_AT); CPC_ClearKey(CPC_KEY_BACKSLASH); }
			continue;
		}
		if (code < 128 && rawmap[code] >= 0) {
			if (down) CPC_SetKey(rawmap[code]); else CPC_ClearKey(rawmap[code]);
		}
	}
	joy = Sys_ReadJoystick() | kb_joy;
	diff = joy ^ joy_prev;
	if (keylog && diff) { fprintf(keylog, "joy %02x raw %08lx\n", joy, Sys_JoyRaw(1)); fflush(keylog); }
	for (i = 0; i < 6; i++)
		if (diff & jk[i].bit) { if (joy & jk[i].bit) CPC_SetKey(jk[i].key); else CPC_ClearKey(jk[i].key); }
	joy_prev = joy;
	return Sys_Poll();
}

/* ---- files ---- */
HOST_FILE_HANDLE Host_OpenFile(const char *n, int Access) { return (HOST_FILE_HANDLE)fopen(n, Access == HOST_FILE_ACCESS_READ ? "rb" : "wb"); }
void Host_CloseFile(HOST_FILE_HANDLE h) { if (h) fclose((FILE *)h); }
int Host_GetFileSize(HOST_FILE_HANDLE h) { FILE *f = (FILE *)h; long p = ftell(f), s; fseek(f, 0, SEEK_END); s = ftell(f); fseek(f, p, SEEK_SET); return (int)s; }
void Host_WriteData(HOST_FILE_HANDLE h, unsigned char *d, unsigned long l) { fwrite(d, 1, l, (FILE *)h); }
void Host_ReadData(HOST_FILE_HANDLE h, unsigned char *d, unsigned long l) { fread(d, 1, l, (FILE *)h); }

BOOL Host_LoadFile(const char *n, unsigned char **pp, unsigned long *pl)
{
	FILE *f = fopen(n, "rb"); long s; unsigned char *p;
	*pp = NULL; *pl = 0;
	if (!f) return FALSE;
	fseek(f, 0, SEEK_END); s = ftell(f); fseek(f, 0, SEEK_SET);
	p = malloc(s);
	if (!p || fread(p, 1, s, f) != (size_t)s) { fclose(f); free(p); return FALSE; }
	fclose(f); *pp = p; *pl = s; return TRUE;
}
BOOL Host_SaveFile(const char *n, unsigned char *p, unsigned long l)
{
	FILE *f = fopen(n, "wb"); if (!f) return FALSE;
	fwrite(p, 1, l, f); fclose(f); return TRUE;
}
BOOL LoadFile(const char *n, unsigned char **pp, unsigned long *pl) { return Host_LoadFile(n, pp, pl); }

/* Argument keywords ignore case, as AmigaDOS does (13 Oct 2026: "Arnold bench"
 * typed in lower case started the emulator instead). Return 0 on a match. */
static int kwncmp(const char *a, const char *kw, int n)
{
	while (n-- > 0) {
		char x = *a++, y = *kw++;
		if (x >= 'a' && x <= 'z') x -= 32;
		if (x != y) return 1;
		if (!x) return 0;
	}
	return 0;
}
static int kwcmp(const char *a, const char *kw) { return kwncmp(a, kw, (int)strlen(kw) + 1); }

static int has_ext(const char *n, const char *ext)
{
	int ln = strlen(n), le = strlen(ext);
	if (ln < le) return 0;
	n += ln - le;
	while (*ext) { char a = *n++, b = *ext++; if (a >= 'A' && a <= 'Z') a += 32; if (a != b) return 0; }
	return 1;
}

/* Arnold [file ...] [NOSOUND] [HIRES] [MENU] [P=prefs] [SKIP=n] [REFRESH=n] [JOYKEYS=0|1|2]: .cpr cartridge (default
 * PROGDIR:roms/system.cpr), .dsk disk in drive A (a second one in B), .sna
 * snapshot. Starts straight into the emulation unless MENU is given or
 * nothing could be loaded. Help brings up the menu; Continue resumes. */
static unsigned char *cart_data;      /* kept: the core may refer to it */

/* Prefs: a text file of key=value lines. PREFS=<file> ToolType or P=<file>
 * argument names it; default PROGDIR:Arnold.prefs. Save Prefs writes it. */
static void prefs_load(void)
{
	FILE *f = fopen(menu.prefs, "r");
	char line[SYS_PATH + 16], *v;
	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		v = strchr(line, '='); if (!v) continue; *v++ = 0;
		if (!strcmp(line, "sound")) menu.sound = atoi(v);
		else if (!strcmp(line, "hires")) menu.hires = atoi(v);
		else if (!strcmp(line, "skip")) skip_max = atoi(v);
		else if (!strcmp(line, "refresh")) refresh = atoi(v);
		else if (!strcmp(line, "joykeys")) joykeys = atoi(v);
		else if (!strcmp(line, "pad")) Sys_PadType = atoi(v) == 1;
		else if (!strcmp(line, "cart") && v[0]) strncpy(menu.cart, v, SYS_PATH - 1);
		else if (!strcmp(line, "diska")) strncpy(menu.diska, v, SYS_PATH - 1);
		else if (!strcmp(line, "diskb")) strncpy(menu.diskb, v, SYS_PATH - 1);
	}
	fclose(f);
}

static void prefs_save(void)
{
	FILE *f = fopen(menu.prefs, "w");
	char msg[SYS_PATH + 32];
	if (!f) { sprintf(msg, "Can't write prefs\n%s", menu.prefs); Sys_MenuError(msg); return; }
	fprintf(f, "; Arnold prefs\nsound=%d\nhires=%d\nskip=%d\nrefresh=%d\njoykeys=%d\npad=%d\ncart=%s\ndiska=%s\ndiskb=%s\n",
		menu.sound, menu.hires, skip_max, refresh, global_joykeys, Sys_PadType, menu.cart, menu.diska, menu.diskb);
	fclose(f);
}

/* Per-game settings: <cartridge>.prefs beside the file (sound, hires), applied
 * when that cartridge is powered on; the global prefs come back otherwise. */
static int global_sound, global_hires;
static void game_prefs_name(char *out) { sprintf(out, "%s.prefs", menu.cart); }

static void game_prefs_apply(void)
{
	char name[SYS_PATH + 8], line[64], *v;
	FILE *f;
	menu.sound = global_sound; menu.hires = global_hires; menu.game_prefs = 0;
	joykeys = global_joykeys;
	game_prefs_name(name);
	f = fopen(name, "r");
	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		v = strchr(line, '='); if (!v) continue; *v++ = 0;
		if (!strcmp(line, "sound")) menu.sound = atoi(v);
		else if (!strcmp(line, "hires")) menu.hires = atoi(v);
		else if (!strcmp(line, "joykeys")) joykeys = atoi(v);
	}
	fclose(f);
	menu.game_prefs = 1;
}

static void game_prefs_save(void)
{
	char name[SYS_PATH + 8], msg[SYS_PATH + 40];
	FILE *f;
	game_prefs_name(name);
	f = fopen(name, "w");
	if (!f) { sprintf(msg, "Can't write game settings\n%s", name); Sys_MenuError(msg); return; }
	fprintf(f, "; Arnold game settings\nsound=%d\nhires=%d\njoykeys=%d\n", menu.sound, menu.hires, joykeys);
	fclose(f);
	menu.game_prefs = 1;
}

static void game_prefs_forget(void)
{
	char name[SYS_PATH + 8];
	game_prefs_name(name);
	remove(name);
	menu.game_prefs = 0;
	menu.sound = global_sound; menu.hires = global_hires; joykeys = global_joykeys;
}

static int load_media(const char *what, const char *name, unsigned char **pp, unsigned long *pl)
{
	char msg[SYS_PATH + 40];
	if (!name[0]) return 0;
	if (Host_LoadFile(name, pp, pl)) return 1;
	sprintf(msg, "Can't load %s\n%s", what, name);
	Sys_MenuError(msg);
	return 0;
}

/* Disks in the drives, by file name. A change made in the menu is applied on
 * Continue and Reset as well as on Power ON, so a multi-disk game can have its
 * next disk put in without a power cycle, and Eject really empties the drive
 * (before 16 Oct 2026 an ejected disk stayed in, and the loaded file was
 * never freed). The core keeps its own copy of the image. */
static char disk_in[2][SYS_PATH];
static void apply_disk(int drive, const char *name)
{
	unsigned char *data; unsigned long len;
	char msg[SYS_PATH + 40];
	if (!strcmp(disk_in[drive], name)) return;
	if (!name[0]) {
		DiskImage_RemoveDisk(drive);
		disk_in[drive][0] = 0;
		Sys_Log("drive %c: ejected", 'A' + drive);
		return;
	}
	if (!load_media("disk", name, &data, &len)) return;
	if (DiskImage_InsertDisk(drive, data, len) != ARNOLD_STATUS_OK) {
		sprintf(msg, "Not a CPC disk image\n%s", name);
		Sys_MenuError(msg);
	} else {
		strncpy(disk_in[drive], name, SYS_PATH - 1);
		Sys_Log("drive %c: '%s', %lu bytes", 'A' + drive, name, len);
	}
	free(data);
}
static void apply_disks(void) { apply_disk(0, menu.diska); apply_disk(1, menu.diskb); }

/* Power ON: cartridge in, Plus hardware, reset, disks in. */
static int power_on(void)
{
	unsigned char *data; unsigned long len;
	/* no cartridge chosen (John ejected it to boot a disk, 17 Oct 2026): the
	 * Plus needs one, so the system cartridge (BASIC + AMSDOS) goes in */
	if (!menu.cart[0]) strcpy(menu.cart, "PROGDIR:roms/system.cpr");
	if (!load_media("cartridge", menu.cart, &data, &len)) return 0;
	Sys_Log("cartridge '%s': %lu bytes", menu.cart, len);
	game_prefs_apply();
	if (cart_data) free(cart_data);
	cart_data = data;
	CPC_SetHardware(CPC_HW_CPCPLUS);
	Cartridge_Insert(data, len);
	Amstrad_DiscInterface_Install();
	Amstrad_RamExpansion_Install();
	/* a power cycle, not just a reset (18 Oct 2026): memory cleared and the
	 * paletted renderer's colours freed, so a game starts as it does from cold
	 * whatever ran before it */
	{ extern void CPC_PowerCycleMemory(void), Palette_Reset(void); CPC_PowerCycleMemory(); Palette_Reset(); }
	CPC_Reset();
	Sys_Log("power on: reset done");
	/* power cycle: both drives reloaded from their files */
	DiskImage_RemoveDisk(0); DiskImage_RemoveDisk(1);
	disk_in[0][0] = disk_in[1][0] = 0;
	apply_disks();
	return 1;
}

extern void (*Render_LineChangedHook)(int SurfaceLine);

static int open_emulation(void)
{
	Render_LineChangedHook = Sys_MarkLineDirty;   /* convert only lines the core changed */
	/* HIRES: 768-pixel lines (CPC mode 2 at full width) on a hires screen; the
	 * default halves them to 384 for a lores screen (modes 0 and 1 exact). */
	if (display_open) { Sys_CloseDisplay(); display_open = 0; }
	Render_SetRenderingAccuracyForWindowedMode(menu.hires ? RENDERING_ACCURACY_HIGH : RENDERING_ACCURACY_LOW);
	if (!Render_SetDisplayWindowed()) { Sys_MenuError("Can't open the AGA screen"); return 0; }
	if (menu.sound && !audio_open && Sys_AudioOpen(AUD_PERIOD, AUD_SAMPLES)) { audio_open = 1; CPC_SetAudioActive(TRUE); }
	if (!menu.sound && audio_open) { Sys_AudioClose(); audio_open = 0; CPC_SetAudioActive(FALSE); }
	CPC_ClearKeyboard();
	Sys_Log("emulation: screen open, sound %s", audio_open ? "on" : "off");
	return 1;
}

int main(int argc, char **argv)
{
	unsigned char *data; unsigned long len;
	const char *sna = NULL;
	int i, want_menu = 0, ndisk = 0, action, nosound = 0, hires = 0, media_given = 0;
	struct SysStart st;

	strcpy(menu.cart, "PROGDIR:roms/system.cpr");
	strcpy(menu.prefs, "PROGDIR:Arnold.prefs");
	menu.sound = 1;
	Sys_LogOpen();
	Sys_LogStart(argc);
	for (i = 1; i < argc; i++) Sys_Log("arg %d: %s", i, argv[i]);
	if (Sys_WBStart(argc, argv, &st)) {
		Sys_Log("wb start: prefs '%s' media '%s' menu %d skip %d", st.prefs, st.media, st.menu, st.skip);
		if (st.prefs[0]) strncpy(menu.prefs, st.prefs, SYS_PATH - 1);
		want_menu = st.menu; nosound = st.nosound; hires = st.hires;
		prefs_load();
		if (st.skip >= 0) skip_max = st.skip;
		if (st.refresh > 0) refresh = st.refresh;
		if (st.joykeys >= 0) joykeys = st.joykeys;
		if (st.pad >= 0) Sys_PadType = st.pad;
		if (has_ext(st.media, ".cpr")) { strncpy(menu.cart, st.media, SYS_PATH - 1); media_given = 1; }
		else if (has_ext(st.media, ".dsk")) { strncpy(menu.diska, st.media, SYS_PATH - 1); menu.diskb[0] = 0; media_given = 1; }
		else if (has_ext(st.media, ".sna")) { sna = st.media; media_given = 1; }
		if (!media_given) want_menu = 1;          /* a bare preset icon: start in the menu */
	} else {
		for (i = 1; i < argc; i++)
			if (!kwncmp(argv[i], "P=", 2)) strncpy(menu.prefs, argv[i] + 2, SYS_PATH - 1);
		prefs_load();
		for (i = 1; i < argc; i++) {
			if (!kwcmp(argv[i], "NOSOUND")) nosound = 1;
			else if (!kwcmp(argv[i], "KEYLOG")) { extern int Sys_MenuKeyLog; keylog = fopen("PROGDIR:keys.log", "w"); Sys_MenuKeyLog = 1; }
			else if (!kwcmp(argv[i], "TURBO")) turbo = 1;
			else if (!kwcmp(argv[i], "PALALL")) Sys_PalAll = 1;
			else if (!kwcmp(argv[i], "ALLLINES")) Sys_AllLines = 1;
			else if (!kwcmp(argv[i], "SAFEFLIP")) Sys_SafeFlip = 1;
			else if (!kwcmp(argv[i], "OSD")) Sys_Osd = 1;
			else if (!kwcmp(argv[i], "NOCD32")) Sys_NoCD32 = 1;
			else if (!kwncmp(argv[i], "SKIP=", 5)) skip_max = atoi(argv[i] + 5);
			else if (!kwncmp(argv[i], "REFRESH=", 8)) refresh = atoi(argv[i] + 8);
			else if (!kwncmp(argv[i], "JOYKEYS=", 8)) joykeys = atoi(argv[i] + 8);
			else if (!kwcmp(argv[i], "CD32") || !kwcmp(argv[i], "PAD=CD32") || !kwcmp(argv[i], "PAD=1")) Sys_PadType = 1;
			else if (!kwcmp(argv[i], "PAD=0") || !kwcmp(argv[i], "PAD=STANDARD")) Sys_PadType = 0;
			else if (!kwcmp(argv[i], "BENCH")) {
				/* machine timings to the Shell and PROGDIR:bench.txt, then exit */
				FILE *bf = fopen("PROGDIR:bench.txt", "w");
				Sys_Bench(stdout);
				if (bf) { Sys_Bench(bf); fclose(bf); }
				return 0;
			}
			else if (!kwncmp(argv[i], "KEYS=", 5)) { if (KeyScript_Load(argv[i] + 5) >= 0) script_on = 1; }
			else if (!kwncmp(argv[i], "SNAPDIR=", 8)) strncpy(snapdir, argv[i] + 8, SYS_PATH - 1);
			else if (!kwcmp(argv[i], "HIRES")) hires = 1;
			else if (!kwcmp(argv[i], "MENU")) want_menu = 1;
			else if (!kwncmp(argv[i], "P=", 2)) ;
			else if (has_ext(argv[i], ".cpr")) { strncpy(menu.cart, argv[i], SYS_PATH - 1); media_given = 1; }
			else if (has_ext(argv[i], ".dsk")) { if (!ndisk) menu.diskb[0] = 0; strncpy(ndisk++ ? menu.diskb : menu.diska, argv[i], SYS_PATH - 1); media_given = 1; }
			else if (has_ext(argv[i], ".sna")) { sna = argv[i]; media_given = 1; }
		}
	}
	Sys_Log("prefs '%s': sound %d hires %d skip %d refresh %d cart '%s'", menu.prefs, menu.sound, menu.hires, skip_max, refresh, menu.cart);
	if (script_on) Sys_MakeDir(snapdir);
	if (nosound) menu.sound = 0;
	if (hires) menu.hires = 1;
	global_sound = menu.sound; global_hires = menu.hires; global_joykeys = joykeys;
	if (!CPCEmulation_CheckEndianness()) { Sys_MenuError("Endianness check failed"); return 10; }
	CPC_Initialise();
	/* the test-mode label's colour: an entry the core leaves alone */
	{ extern void Render_MarkPaletteEntryForHostUse(int); Render_MarkPaletteEntryForHostUse(SYS_OSD_PEN); }
	if (Sys_AllLines || Sys_SafeFlip || Sys_PalAll) Sys_Osd = 1;   /* diagnostics always show their label */
	CPCEmulation_InitialiseDefaultSetup();
	Sys_Log("core initialised");
	CPC_SetFrameSkip(0);
	CPC_SkipNextFrameHook = skip_hook;

	action = want_menu ? SYS_MENU_QUIT : SYS_MENU_POWER;
	if (!want_menu && !power_on()) want_menu = 1;
	if (!want_menu && sna && load_media("snapshot", sna, &data, &len)) Snapshot_Insert(data, len);

	for (;;) {
		if (want_menu) {
			Sys_Log("menu");
			menu.skip = skip_max; menu.refresh = refresh; menu.joykeys = joykeys; menu.pad = Sys_PadType;
			action = Sys_Menu(&menu);
			skip_max = menu.skip; refresh = menu.refresh > 0 ? menu.refresh : 1; joykeys = menu.joykeys; Sys_PadType = menu.pad;
			Sys_Log("menu action %d", action);
			if (action == SYS_MENU_QUIT) break;
			if (!menu.game_prefs) { global_sound = menu.sound; global_hires = menu.hires; global_joykeys = joykeys; }
			if (action == SYS_MENU_SAVE) { int gs = menu.sound, gh = menu.hires; if (menu.game_prefs) { menu.sound = global_sound; menu.hires = global_hires; } else global_joykeys = joykeys; prefs_save(); menu.sound = gs; menu.hires = gh; continue; }
			if (action == SYS_MENU_SAVEGAME) { game_prefs_save(); continue; }
			if (action == SYS_MENU_FORGETGAME) { game_prefs_forget(); continue; }
			if (action == SYS_MENU_SHOT) { screenshot(); continue; }
			if (action == SYS_MENU_POWER && !power_on()) continue;
			if (action == SYS_MENU_RESET) { if (!menu.can_continue && !power_on()) continue; CPC_Reset(); }
			if (action == SYS_MENU_CONTINUE || action == SYS_MENU_RESET) apply_disks();   /* disk swap */
		}
		if (!open_emulation()) { want_menu = 1; continue; }
		menu.can_continue = 1;
		Sys_Log("running");
		CPCEmulation_Run();                 /* returns when Help is pressed (or a script QUIT) */
		Sys_Log("stopped after %lu frames", Sys_Millis() / 20);
		Sys_AudioPause();
		Sys_CloseDisplay(); display_open = 0;
		if (script_quit) {
			char name[SYS_PATH + 8]; FILE *d;
			sprintf(name, "%s/done", snapdir);
			d = fopen(name, "w");
			if (d) { fprintf(d, "frames=%ld\n", script_frame); perf_report(d); fclose(d); }
			perf_report(stdout);
			break;
		}
		want_menu = 1;
	}
	if (audio_open) Sys_AudioClose();
	if (display_open) Sys_CloseDisplay();
	Sys_Log("exit");
	Sys_LogClose();
	return 0;
}
