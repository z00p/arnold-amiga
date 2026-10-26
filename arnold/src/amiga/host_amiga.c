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
#include "../cpc/diskimage/dskwrite.h"
#include "../cpc/cassette.h"
#include "../cpc/tzx.h"
#include "amiga_sys.h"
#include "screenshot.h"
#include "keyscript.h"
#include "keymaplogic.h"
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
static int global_diskwrite = 1;      /* Write disks back: prefs diskwrite=, DISKWRITE=; a game's own prefs can turn it off */
static int global_fasttape = 1;       /* Fast tape: prefs fasttape=, FASTTAPE=; a game's own prefs can turn it off */
static int global_tapesound;   /* Tape sound (25 Oct 2026): prefs tapesound=, TAPESOUND=; per game too */
/* Tapes (24 Oct 2026). The core plays a .cdt (cpc/tzxold.c) while the motor
 * runs: port B bit 7 follows the pulses, timed by the Z80's cycles. Fast tape:
 * while the motor runs and the tape has blocks left, frames are emulated but
 * not drawn (one in TAPE_SHOW is, so the loading screen and the border show
 * progress), there is no 50 Hz wait and Paula gets silence, so a tape that
 * takes minutes loads in seconds. The emulation itself is the same frame for
 * frame (only drawing and pacing change), so picky loaders still work. */
#define TAPE_SHOW 25
static char tape_in[SYS_PATH];        /* the tape the core has, by file name */
static int tape_mute;                 /* this frame's sound block is replaced by silence */
static unsigned long tape_fast_count, tape_fast_frames;
static int tape_fast_active(void)
{
	return menu.fasttape && tape_in[0] && CPC_GetTapeMotor() && TapeImage_IsPlayable();
}
static int display_open, audio_open;
static int aud_period, aud_int;       /* how the open audio was set up (reopened when the timing changes) */
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
static int gx_autostart;   /* frames until the GX4000 auto-start presses f2 (0 = off) */
static long help_after;    /* test: HELPAFTER=n presses Help once after n frames (0 = off) */
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

/* Per-frame figures (vscroll, 20 Oct 2026; reassembler N4): for each measured
 * frame the work (wall time minus waits and snaps), the c2p time and the chip
 * longs written, so heavy moments (Sonic's springs) can be found by frame
 * number. Written to <snapdir>/frames.txt at the script's QUIT; the done file
 * gets the maximum and a histogram. */
#define FL_MAX 4000
static unsigned short fl_work[FL_MAX], fl_c2p[FL_MAX], fl_longs[FL_MAX];
static unsigned char fl_shown[FL_MAX];
static long fl_n;
static unsigned long fl_t, fl_wait, fl_c2pus, fl_lng, fl_snap, fl_frames;
static void framelog_tick(void)
{
	unsigned long t = Sys_Micros(), w, c, l;
	long i = script_frame - PERF_SKIP - 1;
	if (i >= 0 && i < FL_MAX) {
		w = (t - fl_t) - (Sys_Perf.wait_us - fl_wait) - (perf_snap_us - fl_snap);
		c = Sys_Perf.c2p_us - fl_c2pus; l = Sys_Perf.c2p_chip_longs - fl_lng;
		fl_work[i] = w > 65535 ? 65535 : w; fl_c2p[i] = c > 65535 ? 65535 : c; fl_longs[i] = l > 65535 ? 65535 : l;
		fl_shown[i] = Sys_Perf.c2p_frames != fl_frames;
		fl_n = i + 1;
	}
	fl_t = t; fl_wait = Sys_Perf.wait_us; fl_c2pus = Sys_Perf.c2p_us; fl_lng = Sys_Perf.c2p_chip_longs; fl_snap = perf_snap_us; fl_frames = Sys_Perf.c2p_frames;
}
static void framelog_report(FILE *f)
{
	static const unsigned long edge[] = {16000, 18000, 20000, 22000, 25000, 30000, 40000};
	unsigned long h[8] = {0,0,0,0,0,0,0,0}, mw = 0, mc = 0, ml = 0, mwf = 0;
	long i; int k;
	char name[SYS_PATH + 16]; FILE *o;
	for (i = 0; i < fl_n; i++) {
		for (k = 0; k < 7 && fl_work[i] >= edge[k]; k++) ;
		h[k]++;
		if (fl_work[i] > mw) { mw = fl_work[i]; mwf = i; }
		if (fl_c2p[i] > mc) mc = fl_c2p[i];
		if (fl_longs[i] > ml) ml = fl_longs[i];
	}
	fprintf(f, "perf work_max_us=%lu at_frame=%ld c2p_max_us=%lu chip_bytes_max=%lu\n", mw, (long)mwf + PERF_SKIP + 1, mc, ml * 4);
	fprintf(f, "perf work_hist_ms <16:%lu 16-18:%lu 18-20:%lu 20-22:%lu 22-25:%lu 25-30:%lu 30-40:%lu >=40:%lu\n", h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
	if (f == stdout) return;
	sprintf(name, "%s/frames.txt", snapdir);
	if (!(o = fopen(name, "w"))) return;
	fprintf(o, "frame work_us c2p_us chip_longs shown\n");
	for (i = 0; i < fl_n; i++) fprintf(o, "%ld %u %u %u %u\n", i + PERF_SKIP + 1, fl_work[i], fl_c2p[i], fl_longs[i], fl_shown[i]);
	fclose(o);
}

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
	fprintf(f, "perf c2p_lines5=%lu c2p_lines6=%lu c2p_clear_planes=%lu chip_bytes_per_shown_frame_est=%lu\n", Sys_Perf.c2p_lines5, Sys_Perf.c2p_lines6, Sys_Perf.c2p_clear_planes,
		((Sys_Perf.c2p_lines - Sys_Perf.c2p_lines5 - Sys_Perf.c2p_lines6) * 8 + Sys_Perf.c2p_lines5 * 5 + Sys_Perf.c2p_lines6 * 6 + Sys_Perf.c2p_clear_planes) * (Sys_Perf.line_bytes / 8) / cf);
	if (Sys_Perf.c2p_chip_longs) fprintf(f, "perf chip_bytes_written_per_shown_frame=%lu\n", Sys_Perf.c2p_chip_longs * 4 / cf);
	fprintf(f, "perf vshift=%d vs_searches=%lu vs_shifts=%lu vs_recentres=%lu vs_us_per_shown_frame=%lu\n", Sys_VShiftOn_Last, Sys_Perf.vs_searches, Sys_Perf.vs_shifts, Sys_Perf.vs_recentres, Sys_Perf.vs_us / cf);
	if (Sys_Audit) fprintf(f, "perf audit_frames=%lu audit_bad_frames=%lu audit_bad_groups=%lu audit_copper_checks=%lu audit_copper_bad=%lu\n",
		Sys_Perf.audit_frames, Sys_Perf.audit_bad_frames, Sys_Perf.audit_bad_groups, Sys_Perf.audit_cop_checks, Sys_Perf.audit_cop_bad);
	framelog_report(f);
}

static void host_key(int code, int down, int qual);
static void script_setkey(int key, int down)
{
	if (key & KS_AMIGA) { host_key(key & 0x7f, down, 0); return; }   /* A:<key>: as if typed on the Amiga */
	if (down) CPC_SetKey(key); else CPC_ClearKey(key);
}

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
		/* a new buffer for each screen (lores/hires may differ): free the last
		 * one, or every Help / Continue or Power ON leaked 104 KB (208 KB hires).
		 * The core takes the pointer from Host_GetGraphicsBufferInfo each frame. */
		if (chunky) free(chunky);
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
	/* Amiga colour 0 also fills the screen surround (everything outside the
	 * picture), so it shows the CPC's border colour, as a real monitor does
	 * out to the tube's edge: blue for BASIC, black for most Plus games
	 * (19 Oct 2026, John). The core's entry 0 itself is black (blanking). */
	extern int Render_PaletteIndexBorder;
	int bi = Render_PaletteIndexBorder;
	if (i >= 0 && i < 256) { hostpal[i][0] = r; hostpal[i][1] = g; hostpal[i][2] = b; }
	if (i == 0 && bi > 0 && bi < 256) { r = hostpal[bi][0]; g = hostpal[bi][1]; b = hostpal[bi][2]; }
	Sys_SetPaletteEntry(i, r, g, b);
	if (i == bi && bi > 0) Sys_SetPaletteEntry(0, r, g, b);
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
void Host_UnLockAudioBuffer(void)
{
	if (tape_mute) memset(audio_tmp, 0, audio_tmp_len);   /* fast tape: silence (the queue drops what can't be played) */
	if (menu.tapesound) TapeAudio_Mix((unsigned short *)audio_tmp, audio_tmp_len / 4);   /* tape sound: the tape level into this block (cpc/cassette.c) */
	Sys_AudioQueue(audio_tmp, audio_tmp_len / 4);
}

/* Once a frame: the core mixes the frame's sound (Host_LockAudioBuffer /
 * Host_UnLockAudioBuffer queue it for Paula), then the 50 Hz wait. The Windows
 * host does the same; without the CPC_UpdateAudio call there is no sound. */
void Host_Throttle(void)
{
	int fast = tape_in[0] && tape_fast_active();
	if (fast != tape_mute) {   /* the log shows each fast stretch (only on changes) */
		static unsigned long t0;
		if (fast) { t0 = Sys_Micros(); Sys_Log("tape: fast on, block %d/%d", TapeImage_GetCurrentBlock() + 1, TapeImage_GetNumBlocks()); }
		else Sys_Log("tape: fast off after %lu frames in %lu ms", tape_fast_frames, (Sys_Micros() - t0) / 1000);
		tape_fast_frames = 0;
	}
	tape_mute = fast;
	{	/* tape sound: only at real speed */
		static int ts_was;
		static unsigned long ts_frames;
		if (menu.tapesound != ts_was) {
			ts_was = menu.tapesound;
			TapeAudio_Reset();
			Sys_Log("tape sound: %s", ts_was ? "on" : "off");
		}
		TapeAudio_On = menu.tapesound && audio_open && tape_in[0] && !fast;
		if (TapeAudio_On && !(++ts_frames % 500)) Sys_Log("tape sound: %lu frames mixed, %lu level changes", ts_frames, TapeAudio_Changes);
	}
	if (audio_open) CPC_UpdateAudio();
	if (fast) { behind_us = 0; tape_fast_frames++; return; }   /* fast tape: no 50 Hz wait */
	behind_us = turbo ? 0 : Sys_WaitFrame();   /* TURBO: run flat out */
}

static int skip_hook(void)
{
	int fixed = refresh > 1 && (refresh_count++ % refresh) != 0;
	if (script_on && KeyScript_SnapAt(script_frame + 1)) { skip_run = 0; return 0; }
	if (tape_in[0] && tape_fast_active()) { skip_run = 0; return (tape_fast_count++ % TAPE_SHOW) != 0; }
	tape_fast_count = 0;
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

/* Key remap (20 Oct 2026, John): Amiga key -> CPC key, from remap= in the prefs
 * or a game's .prefs, or REMAP= (ToolType or argument), e.g. "RSHIFT:M,RALT:SPACE".
 * The CPC has one Shift in its matrix, so this is how Right Shift can be a
 * different key (Pinball Dreams: right flipper = M). A remapped key gives only
 * its new key (it comes before the joy keys); no cost when nothing is remapped. */
static char remap_str[128], global_remap[128];
static short remap_key[128];
static int remap_on;
static int amiga_key_code(const char *s)
{
	int i, id;
	if ((i = KeyMap_AmigaNameCode(s)) >= 0) return i;
	if (s[0] == 'F' && s[1] >= '1' && s[1] <= '9') { i = atoi(s + 1); if (i >= 1 && i <= 10) return 0x50 + i - 1; }
	if (strlen(s) == 1 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= '0' && s[0] <= '9'))) {
		id = KeyScript_KeyId(s);   /* a letter or digit: where the Amiga has that CPC key */
		for (i = 1; i < 0x40; i++) if (rawmap[i] == id) return i;
	}
	return -1;
}
/* rawkey -> name for the key map editor (names as remap= takes them); NULL if the key can't be remapped */
const char *Host_AmigaKeyName(int code, char *buf)
{
	const char *n = KeyMap_AmigaCodeName(code);
	int i;
	if (n) return n;
	if (code >= 0x50 && code <= 0x59) { sprintf(buf, "F%d", code - 0x50 + 1); return buf; }
	if (code >= 0 && code < 0x40 && rawmap[code] >= 0)
		for (i = 0; i < 36; i++) {
			buf[0] = (char)(i < 26 ? 'A' + i : '0' + i - 26); buf[1] = 0;
			if (KeyScript_KeyId(buf) == rawmap[code] && amiga_key_code(buf) == code) return buf;
		}
	return NULL;
}
static void remap_parse(const char *str)
{
	char buf[128], *p, *c;
	int i, a, k;
	for (i = 0; i < 128; i++) remap_key[i] = -1;
	remap_on = 0;
	strncpy(remap_str, str, sizeof remap_str - 1); remap_str[sizeof remap_str - 1] = 0;
	strcpy(buf, remap_str);
	for (p = buf; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
	for (p = strtok(buf, ", "); p; p = strtok(NULL, ", ")) {
		c = strchr(p, ':'); if (!c) continue; *c++ = 0;
		a = amiga_key_code(p); k = KeyScript_KeyId(c);
		/* the target must be a CPC key: "A:<name>" (a script's Amiga key) would be
		 * KS_AMIGA | rawkey, far past the CPC keyboard matrix in CPC_SetKey */
		if (a < 0 || k < 0 || (k & KS_AMIGA)) { Sys_Log("remap: can't map '%s:%s'", p, c); continue; }
		remap_key[a] = k; remap_on = 1;
	}
}

static void host_key(int code, int down, int qual)
{
	if (keylog) { fprintf(keylog, "%02x %s q=%04x\n", code, down ? "down" : "up", qual); fflush(keylog); }
	if (code == RAW_HELP) { if (down) Sys_RequestQuit(); return; }
	if (code >= 128) return;
	if (remap_on && remap_key[code] >= 0) { if (down) CPC_SetKey(remap_key[code]); else CPC_ClearKey(remap_key[code]); return; }
	if (joykey(code, down)) return;
	if (menu.running_model == SYS_MODEL_GX4000) return;   /* the console has no keyboard */
	if (code == 0x0d) {
		/* the Amiga's \ key: plain = the CPC's \, with Shift = the CPC's |
		 * (Shift+@), which RSX commands such as |CPM and Orion Prime's loader
		 * need (17 Oct 2026). Both cleared on release. */
		if (down) CPC_SetKey((qual & 3) ? CPC_KEY_AT : CPC_KEY_BACKSLASH);
		else { CPC_ClearKey(CPC_KEY_AT); CPC_ClearKey(CPC_KEY_BACKSLASH); }
		return;
	}
	if (rawmap[code] >= 0) {
		if (down) CPC_SetKey(rawmap[code]); else CPC_ClearKey(rawmap[code]);
	}
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
		if (script_frame == PERF_SKIP) { perf_t0 = Sys_Micros(); memset(&Sys_Perf, 0, sizeof Sys_Perf); perf_snap_us = 0; fl_n = 0; }
		if (script_frame >= PERF_SKIP) framelog_tick();
		if (fl & KS_SNAP) { unsigned long t = Sys_Micros(); script_snap(); perf_snap_us += Sys_Micros() - t; }
		if (fl & KS_QUIT) { script_quit = 1; perf_t1 = Sys_Micros(); Sys_RequestQuit(); }
		script_frame++;
	}
	{ static long hb; if (++hb % 50 == 0) Sys_Heartbeat(hb, behind_us); }
	/* GX4000 with the Plus system cartridge (19 Oct 2026, John): that cartridge
	 * shows the f1 BASIC / f2 Burnin' Rubber menu (the console came with a
	 * game-only cartridge instead), and a console has no keyboard: press f2
	 * for it, held for a few frames once the menu is up */
	if (gx_autostart > 0) {
		gx_autostart--;
		if (gx_autostart == 6) CPC_SetKey(CPC_KEY_F2);
		if (gx_autostart == 0) CPC_ClearKey(CPC_KEY_F2);
	}
	if (help_after > 0 && --help_after == 0) Sys_RequestQuit();
	if (Sys_Poll()) return TRUE;
	while (Sys_GetKey(&code, &down, &qual)) host_key(code, down, qual);
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

/* Arnold [file ...] [NOSOUND] [HIRES] [TALL] [WIDE] [MENU] [P=prefs] [SKIP=n] [REFRESH=n] [JOYKEYS=0|1|2]
 * [PAD=0|CD32] [SPEED=50|DISPLAY] [VIDEO=AUTO|PAL|NTSC] [MODEL=464|664|6128|PLUS|GX4000] [DISKWRITE=0|1]: .cpr cartridge (default
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
		else if (!strcmp(line, "tall")) menu.tall = atoi(v);
		else if (!strcmp(line, "wide")) menu.wide = atoi(v);
		else if (!strcmp(line, "skip")) skip_max = atoi(v);
		else if (!strcmp(line, "refresh")) refresh = atoi(v);
		else if (!strcmp(line, "joykeys")) joykeys = atoi(v);
		else if (!strcmp(line, "remap")) remap_parse(v);
		else if (!strcmp(line, "pad")) Sys_PadType = atoi(v) == 1;
		else if (!strcmp(line, "speed")) { int k = Sys_ParseSpeed(v); if (k >= 0) menu.speed = k; }
		else if (!strcmp(line, "video")) { int k = Sys_ParseVideo(v); if (k >= 0) menu.video = k; }
		else if (!strcmp(line, "model")) { int k = Sys_ParseModel(v); if (k >= 0) menu.model = k; }
		else if (!strcmp(line, "cart") && v[0]) strncpy(menu.cart, v, SYS_PATH - 1);
		else if (!strcmp(line, "diska")) strncpy(menu.diska, v, SYS_PATH - 1);
		else if (!strcmp(line, "diskb")) strncpy(menu.diskb, v, SYS_PATH - 1);
		else if (!strcmp(line, "diskwrite")) menu.diskwrite = atoi(v) ? 1 : 0;
		else if (!strcmp(line, "fasttape")) menu.fasttape = atoi(v) ? 1 : 0;
		else if (!strcmp(line, "tapesound")) menu.tapesound = atoi(v) ? 1 : 0;
		else if (!strcmp(line, "tape")) strncpy(menu.tape, v, SYS_PATH - 1);
	}
	fclose(f);
}

static void prefs_save(void)
{
	FILE *f = fopen(menu.prefs, "w");
	char msg[SYS_PATH + 32];
	if (!f) { sprintf(msg, "Can't write prefs\n%s", menu.prefs); Sys_MenuError(msg); return; }
	fprintf(f, "; Arnold prefs\nsound=%d\nhires=%d\nskip=%d\nrefresh=%d\njoykeys=%d\npad=%d\nspeed=%s\nvideo=%s\nmodel=%s\ncart=%s\ndiska=%s\ndiskb=%s\n",
		menu.sound, menu.hires, skip_max, refresh, global_joykeys, Sys_PadType,
		Sys_SpeedNames[menu.speed ? 1 : 0], Sys_VideoNames[menu.video >= 0 && menu.video <= 2 ? menu.video : 0],
		Sys_ModelNames[menu.model >= 0 && menu.model < SYS_MODELS ? menu.model : SYS_MODEL_PLUS], menu.cart, menu.diska, menu.diskb);
	fprintf(f, "tall=%d\nwide=%d\n", menu.tall, menu.wide);
	fprintf(f, "diskwrite=%d\n", global_diskwrite);
	fprintf(f, "fasttape=%d\ntape=%s\n", global_fasttape, menu.tape);
	fprintf(f, "tapesound=%d\n", global_tapesound);
	if (global_remap[0]) fprintf(f, "remap=%s\n", global_remap);
	fclose(f);
}

/* Per-game settings: <cartridge>.prefs beside the file (sound, hires, joy keys,
 * full height, key remap), applied when that game is powered on; the global
 * prefs come back otherwise. A disk game (the Plus with the system cartridge,
 * or a plain CPC, with a disk in drive A) uses <disk A>.prefs (20 Oct 2026). */
static int global_sound, global_hires, global_tall, global_wide;
static void game_prefs_name(char *out)
{
	const char *c = menu.cart, *e = c + strlen(c);
	int plus = menu.model == SYS_MODEL_PLUS || menu.model == SYS_MODEL_GX4000;
	int system = e - c >= 10 && !kwcmp(e - 10, "SYSTEM.CPR");
	/* a tape game (24 Oct 2026): <tape>.prefs when there is a tape and no disk
	 * in drive A (or a 464, which has no drive) */
	if (menu.tape[0] && (!plus || system) && (!menu.diska[0] || menu.model == SYS_MODEL_464)) { sprintf(out, "%s.prefs", menu.tape); return; }
	sprintf(out, "%s.prefs", menu.diska[0] && (!plus || system) ? menu.diska : menu.cart);
}

static void game_prefs_apply(void)
{
	char name[SYS_PATH + 8], line[SYS_PATH + 16], *v;   /* a remap= line can be 6 + 127 characters */
	FILE *f;
	menu.sound = global_sound; menu.hires = global_hires; menu.game_prefs = 0; menu.tall = global_tall; menu.wide = global_wide;
	menu.diskwrite = global_diskwrite;
	menu.fasttape = global_fasttape; menu.tapesound = global_tapesound;
	joykeys = global_joykeys;
	if (strcmp(remap_str, global_remap)) remap_parse(global_remap);
	game_prefs_name(name);
	f = fopen(name, "r");
	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		v = strchr(line, '='); if (!v) continue; *v++ = 0;
		if (!strcmp(line, "sound")) menu.sound = atoi(v);
		else if (!strcmp(line, "hires")) menu.hires = atoi(v);
		else if (!strcmp(line, "joykeys")) joykeys = atoi(v);
		else if (!strcmp(line, "tall")) menu.tall = atoi(v);
		else if (!strcmp(line, "wide")) menu.wide = atoi(v);
		else if (!strcmp(line, "remap")) remap_parse(v);
		else if (!strcmp(line, "diskwrite")) menu.diskwrite = atoi(v) ? 1 : 0;
		else if (!strcmp(line, "fasttape")) menu.fasttape = atoi(v) ? 1 : 0;
		else if (!strcmp(line, "tapesound")) menu.tapesound = atoi(v) ? 1 : 0;
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
	fprintf(f, "; Arnold game settings\nsound=%d\nhires=%d\njoykeys=%d\ntall=%d\nwide=%d\n", menu.sound, menu.hires, joykeys, menu.tall, menu.wide);
	if (remap_str[0]) fprintf(f, "remap=%s\n", remap_str);
	if (menu.diskwrite != global_diskwrite) fprintf(f, "diskwrite=%d\n", menu.diskwrite);
	if (menu.fasttape != global_fasttape) fprintf(f, "fasttape=%d\n", menu.fasttape);
	if (menu.tapesound != global_tapesound) fprintf(f, "tapesound=%d\n", menu.tapesound);
	fclose(f);
	menu.game_prefs = 1;
}

static void game_prefs_forget(void)
{
	char name[SYS_PATH + 8];
	game_prefs_name(name);
	remove(name);
	menu.game_prefs = 0;
	menu.sound = global_sound; menu.hires = global_hires; joykeys = global_joykeys; menu.tall = global_tall; menu.wide = global_wide;
	menu.diskwrite = global_diskwrite;
	menu.fasttape = global_fasttape; menu.tapesound = global_tapesound;
	remap_parse(global_remap);
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
/* Disk write-back (22 Oct 2026): the file as loaded (or as last written) is
 * kept per drive as the template for cpc/diskimage/dskwrite.c. A drive the
 * machine changed is written back when the menu opens (Help), on Eject, when
 * another disk goes in, on Power ON and on Exit, if "Write disks back" is on.
 * A write that fails (write-protected file or volume, disk full) is logged and
 * shown once in the menu status line; the changes then stay in memory only. */
static unsigned char *disk_img[2];
static unsigned long disk_len[2];
static int disk_wbfail[2];
static void flush_disk(int drive, const char *when)
{
	char msg[DSKWRITE_MSG];
	int r;
	if (!menu.diskwrite || !disk_in[drive][0] || !disk_img[drive] || disk_wbfail[drive]) return;
	if (!DiskImage_IsImageDirty(drive)) return;
	r = DskWrite_Flush(drive, disk_in[drive], &disk_img[drive], &disk_len[drive], msg);
	if (r == 0) return;
	Sys_Log("drive %c (%s): '%s' %s", 'A' + drive, when, disk_in[drive], msg);
	if (r < 0) {
		disk_wbfail[drive] = 1;
		sprintf(menu.status, "Disk %c not saved: ", 'A' + drive);
		strncat(menu.status, msg + 13, sizeof menu.status - strlen(menu.status) - 1);   /* past "not written: " */
	}
}
static void flush_disks(const char *when) { flush_disk(0, when); flush_disk(1, when); }
static void drop_image(int drive)
{
	if (disk_img[drive]) free(disk_img[drive]);
	disk_img[drive] = NULL; disk_len[drive] = 0; disk_wbfail[drive] = 0;
}
static void apply_disk(int drive, const char *name)
{
	unsigned char *data; unsigned long len;
	char msg[SYS_PATH + 40];
	if (!strcmp(disk_in[drive], name)) return;
	flush_disk(drive, name[0] ? "disk change" : "eject");
	if (!name[0]) {
		DiskImage_RemoveDisk(drive);
		disk_in[drive][0] = 0;
		drop_image(drive);
		Sys_Log("drive %c: ejected", 'A' + drive);
		return;
	}
	if (!load_media("disk", name, &data, &len)) return;
	if (DiskImage_InsertDisk(drive, data, len) != ARNOLD_STATUS_OK) {
		sprintf(msg, "Not a CPC disk image\n%s", name);
		Sys_MenuError(msg);
		free(data);
	} else {
		strncpy(disk_in[drive], name, SYS_PATH - 1);
		drop_image(drive);
		disk_img[drive] = data; disk_len[drive] = len;   /* kept: the write-back template */
		Sys_Log("drive %c: '%s', %lu bytes", 'A' + drive, name, len);
	}
}
static void apply_disks(void) { apply_disk(0, menu.diska); apply_disk(1, menu.diskb); }

/* The tape in the deck, by file name, applied like the disks: on Power ON
 * (always from the start), and on Continue / Reset when the menu changed it
 * (a new tape, Eject) or asked for Rewind. Reset keeps the tape position, as
 * on the real machine. Only .cdt (TZX) images; tape writing is not supported. */
static void tape_pos(void)
{
	if (tape_in[0]) sprintf(menu.tapepos, "Tape %d/%d  ", TapeImage_GetCurrentBlock() + (TapeImage_IsPlayable() ? 1 : 0), TapeImage_GetNumBlocks());
	else menu.tapepos[0] = 0;
}
static void apply_tape(int force)
{
	unsigned char *data; unsigned long len;
	char msg[SYS_PATH + 40];
	const char *name = menu.tape;
	if (!force && !strcmp(tape_in, name)) {
		if (menu.tape_rewind && tape_in[0]) { TapeImage_RewindToStart(); Sys_Log("tape: rewound"); }
		menu.tape_rewind = 0;
		return;
	}
	menu.tape_rewind = 0;
	if (tape_in[0]) { TapeImage_Remove(); CPC_SetCassetteType(CASSETTE_TYPE_NONE); tape_in[0] = 0; }
	if (!name[0]) { Sys_Log("tape: ejected"); return; }
	if (!load_media("tape", name, &data, &len)) return;
	if (TapeImage_Insert(data, len) != ARNOLD_STATUS_OK) {
		CPC_SetCassetteType(CASSETTE_TYPE_NONE);
		sprintf(msg, "Not a CPC tape image (.cdt)\n%s", name);
		Sys_MenuError(msg);
	} else {
		strncpy(tape_in, name, SYS_PATH - 1);
		Sys_Log("tape: '%s', %lu bytes, %d blocks", name, len, TapeImage_GetNumBlocks());
	}
	free(data);   /* the core keeps its own copy */
}

/* System ROMs of the plain CPCs (19 Oct 2026), from the drawer like
 * system.cpr: roms/cpc464, cpc664, cpc6128 (os.rom, basic.rom) and
 * roms/amsdos.rom; the English ROMs Arnold ships. Loaded on the Power ON that
 * needs them and kept (the core points into them). */
static unsigned char *rom_os, *rom_basic, *rom_dos;
static int rom_model = -1;
static int load_rom(const char *name, unsigned char **pp)
{
	unsigned char *d; unsigned long l;
	if (!load_media("system ROM", name, &d, &l)) return 0;
	if (l < 16384) { char msg[SYS_PATH + 40]; sprintf(msg, "Not a 16K ROM\n%s", name); Sys_MenuError(msg); free(d); return 0; }
	if (*pp) free(*pp);
	*pp = d;
	return 1;
}
static int load_cpc_roms(int model)
{
	static const char *dir[] = { "cpc464", "cpc664", "cpc6128" };
	char n[64];
	unsigned char *os = NULL, *basic = NULL, *dos = NULL;
	if (rom_model == model) return 1;
	/* all three into new buffers first: the running machine (Continue is still
	 * offered when this Power ON fails) reads the old ones through the core's
	 * pointers, so they are only replaced once every file has loaded; power_on
	 * hands the new ones to the core straight after */
	sprintf(n, "PROGDIR:roms/%s/os.rom", dir[model]);
	if (!load_rom(n, &os)) return 0;
	sprintf(n, "PROGDIR:roms/%s/basic.rom", dir[model]);
	if (!load_rom(n, &basic)) { free(os); return 0; }
	if (model != SYS_MODEL_464 && !rom_dos && !load_rom("PROGDIR:roms/amsdos.rom", &dos)) { free(os); free(basic); return 0; }
	if (rom_os) free(rom_os);
	if (rom_basic) free(rom_basic);
	rom_os = os; rom_basic = basic;
	if (dos) rom_dos = dos;
	rom_model = model;
	Sys_Log("system ROMs: %s loaded", dir[model]);
	return 1;
}

/* Power ON: the chosen model's hardware, its ROMs or the cartridge, reset,
 * disks in. Models (19 Oct 2026): 464 (64K, tape only: no disc interface),
 * 664 (64K, disc), 6128 (128K, disc), 6128 Plus (cartridge, 128K, disc) and
 * GX4000 (cartridge, 64K, no disc interface, no keyboard). */
static int power_on(void)
{
	unsigned char *data = NULL; unsigned long len = 0;
	int model = menu.model >= 0 && menu.model < SYS_MODELS ? menu.model : SYS_MODEL_PLUS;
	int plus = model == SYS_MODEL_PLUS || model == SYS_MODEL_GX4000;
	flush_disks("power on");   /* before the next game's prefs can change diskwrite */
	if (plus) {
		/* no cartridge chosen (John ejected it to boot a disk, 17 Oct 2026): the
		 * Plus needs one, so the system cartridge (BASIC + AMSDOS) goes in */
		if (!menu.cart[0]) strcpy(menu.cart, "PROGDIR:roms/system.cpr");
		if (!load_media("cartridge", menu.cart, &data, &len)) return 0;
		Sys_Log("cartridge '%s': %lu bytes", menu.cart, len);
		{
			/* Cartridge_Insert ignores a file that is not a cartridge and keeps the
			 * last one, so check it before anything changes (it would run the
			 * previous game, or none, without a word) */
			extern BOOL Cartridge_ValidateCartridge(const unsigned char *pData, int FileSize);
			if (len < 12 || !Cartridge_ValidateCartridge(data, (int)len)) {
				char msg[SYS_PATH + 40];
				sprintf(msg, "Not a CPC+ cartridge (.cpr)\n%s", menu.cart);
				Sys_MenuError(msg);
				free(data);
				return 0;
			}
		}
	} else if (!load_cpc_roms(model)) return 0;
	game_prefs_apply();
	if (plus) {
		if (cart_data) free(cart_data);
		cart_data = data;
		CPC_SetHardware(CPC_HW_CPCPLUS);
		Cartridge_Insert(data, len);
	} else {
		CPC_SetOSRom(rom_os);
		CPC_SetBASICRom(rom_basic);
		CPC_SetDOSRom(model == SYS_MODEL_464 ? NULL : rom_dos);
		CPC_SetHardware(CPC_HW_CPC);
	}
	if (model == SYS_MODEL_464 || model == SYS_MODEL_GX4000) Amstrad_DiscInterface_DeInstall();
	else Amstrad_DiscInterface_Install();
	if (model == SYS_MODEL_6128 || model == SYS_MODEL_PLUS) Amstrad_RamExpansion_Install();
	else Amstrad_RamExpansion_DeInstall();
	/* a power cycle, not just a reset (18 Oct 2026): memory cleared and the
	 * paletted renderer's colours freed, so a game starts as it does from cold
	 * whatever ran before it */
	{ extern void CPC_PowerCycleMemory(void), Palette_Reset(void); CPC_PowerCycleMemory(); Palette_Reset(); }
	CPC_Reset();
	menu.running_model = model;
	gx_autostart = (model == SYS_MODEL_GX4000 && strstr(menu.cart, "system.cpr")) ? 150 : 0;   /* about 3 s, see Host_ProcessSystemEvents */
	Sys_Log("power on: %s, reset done", Sys_ModelNames[model]);
	/* power cycle: both drives reloaded from their files */
	DiskImage_RemoveDisk(0); DiskImage_RemoveDisk(1);
	disk_in[0][0] = disk_in[1][0] = 0;
	drop_image(0); drop_image(1);
	apply_disks();
	apply_tape(1);   /* the tape from the start */
	return 1;
}

extern void (*Render_LineChangedHook)(int SurfaceLine);

static int open_emulation(void)
{
	Render_LineChangedHook = Sys_MarkLineDirty;   /* convert only lines the core changed */
	/* HIRES: 768-pixel lines (CPC mode 2 at full width) on a hires screen; the
	 * default halves them to 384 for a lores screen (modes 0 and 1 exact). */
	if (display_open) { Sys_CloseDisplay(); display_open = 0; }
	Sys_WideScreen = menu.wide;
	Sys_TallScreen = menu.tall;   /* TALL: all 272 lines (amiga_sys.c) */
	Render_SetRenderingAccuracyForWindowedMode(menu.hires ? RENDERING_ACCURACY_HIGH : RENDERING_ACCURACY_LOW);
	if (!Render_SetDisplayWindowed()) { Sys_MenuError("Can't open the AGA screen"); return 0; }
	/* Timing (19 Oct 2026). A 50 Hz display: one CPC frame per VBlank and Paula
	 * fed from the VBlank, as tuned on PAL. Any other rate (NTSC 60 Hz):
	 * - Game speed "50 Hz": the E-clock paces the frames, Paula's own interrupt
	 *   feeds it, and the period makes a block of 444 samples last one CPC
	 *   frame (19968 us) for this machine's chip clock; the frame time is set
	 *   to that block's exact length so sound and emulation never drift.
	 * - "Follow display": one frame per VBlank (60 Hz: 20% fast, as before),
	 *   with the period shortened so the blocks keep up. */
	{
		unsigned long clk = Sys_ChipClock(), fus, target;
		int period, audint, timer;
		if (Sys_Display50Hz()) { period = AUD_PERIOD; audint = 0; timer = 0; fus = 20000; }
		else {
			target = menu.speed ? Sys_DisplayFrameUs : 19968;
			period = (int)(((unsigned long long)target * clk + 444ULL * 500000ULL) / (444ULL * 1000000ULL));
			if (period < 124) period = 124;
			audint = 1; timer = !menu.speed;
			fus = menu.speed ? Sys_DisplayFrameUs : (unsigned long)(444ULL * period * 1000000ULL / clk);
		}
		Sys_SetPacing(timer, fus);
		if (audio_open && (period != aud_period || audint != aud_int)) { Sys_AudioClose(); audio_open = 0; }
		aud_period = period; aud_int = audint;
	}
	if (menu.sound && !audio_open && Sys_AudioOpen(aud_period, AUD_SAMPLES, aud_int)) { audio_open = 1; CPC_SetAudioActive(TRUE); }
	if (!menu.sound && audio_open) { Sys_AudioClose(); audio_open = 0; CPC_SetAudioActive(FALSE); }
	CPC_ClearKeyboard();
	/* and the joystick state with it: a joy key held when Help was pressed is
	 * released in the menu, which kept its bit in kb_joy (the real joystick's
	 * same direction then did nothing until that key was tapped again);
	 * joy_prev 0: the next frame sets whatever is really held */
	kb_joy = 0; joy_prev = 0;
	Sys_Log("emulation: screen open, sound %s", audio_open ? "on" : "off");
	return 1;
}

int main(int argc, char **argv)
{
	unsigned char *data; unsigned long len;
	const char *sna = NULL;
	int i, want_menu = 0, ndisk = 0, action, nosound = 0, hires = 0, tall = 0, wide = 0, media_given = 0;
	struct SysStart st;

	KeyScript_AmigaKey = amiga_key_code;   /* before KEYS= loads a script */
	strcpy(menu.cart, "PROGDIR:roms/system.cpr");
	strcpy(menu.prefs, "PROGDIR:Arnold.prefs");
	menu.sound = 1;
	menu.diskwrite = 1;
	menu.fasttape = 1; menu.tapesound = 0;
	menu.model = SYS_MODEL_PLUS; menu.running_model = -1;
	Sys_LogOpen();
	Sys_LogStart(argc);
	for (i = 1; i < argc; i++) Sys_Log("arg %d: %s", i, argv[i]);
	if (Sys_WBStart(argc, argv, &st)) {
		Sys_Log("wb start: prefs '%s' media '%s' menu %d skip %d", st.prefs, st.media, st.menu, st.skip);
		if (st.prefs[0]) strncpy(menu.prefs, st.prefs, SYS_PATH - 1);
		want_menu = st.menu; nosound = st.nosound; hires = st.hires;
		prefs_load();
		if (st.tall) menu.tall = 1;
		if (st.wide) menu.wide = 1;
		if (st.skip >= 0) skip_max = st.skip;
		if (st.refresh > 0) refresh = st.refresh;
		if (st.joykeys >= 0) joykeys = st.joykeys;
		if (st.remap[0]) remap_parse(st.remap);
		if (st.pad >= 0) Sys_PadType = st.pad;
		if (st.speed >= 0) menu.speed = st.speed;
		if (st.video >= 0) menu.video = st.video;
		if (st.model >= 0) menu.model = st.model;
		if (st.diskwrite >= 0) menu.diskwrite = st.diskwrite;
		if (st.fasttape >= 0) menu.fasttape = st.fasttape;
		if (st.tapesound >= 0) menu.tapesound = st.tapesound;
		if (st.tape[0]) strncpy(menu.tape, st.tape, SYS_PATH - 1);
		if (has_ext(st.media, ".cpr")) { strncpy(menu.cart, st.media, SYS_PATH - 1); media_given = 1; }
		else if (has_ext(st.media, ".dsk")) { strncpy(menu.diska, st.media, SYS_PATH - 1); menu.diskb[0] = 0; media_given = 1; }
		else if (has_ext(st.media, ".sna")) { sna = st.media; media_given = 1; }
		else if (has_ext(st.media, ".cdt")) { strncpy(menu.tape, st.media, SYS_PATH - 1); media_given = 1; }
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
			else if (!kwcmp(argv[i], "NOVSHIFT")) Sys_NoVShift = 1;   /* vscroll A/B: plain buffers */
			else if (!kwcmp(argv[i], "AUDIT")) Sys_Audit = 1;         /* check chip RAM and the copper after every frame (slow) */
			else if (!kwcmp(argv[i], "SAFEFLIP")) Sys_SafeFlip = 1;
			else if (!kwcmp(argv[i], "OSD")) Sys_Osd = 1;
			else if (!kwcmp(argv[i], "NOCD32")) Sys_NoCD32 = 1;
			else if (!kwncmp(argv[i], "SKIP=", 5)) skip_max = atoi(argv[i] + 5);
			else if (!kwncmp(argv[i], "REFRESH=", 8)) refresh = atoi(argv[i] + 8);
			else if (!kwncmp(argv[i], "JOYKEYS=", 8)) joykeys = atoi(argv[i] + 8);
			else if (!kwncmp(argv[i], "REMAP=", 6)) remap_parse(argv[i] + 6);
			else if (!kwcmp(argv[i], "CD32") || !kwcmp(argv[i], "PAD=CD32") || !kwcmp(argv[i], "PAD=1")) Sys_PadType = 1;
			else if (!kwcmp(argv[i], "PAD=0") || !kwcmp(argv[i], "PAD=STANDARD")) Sys_PadType = 0;
			else if (!kwncmp(argv[i], "SPEED=", 6)) { int k = Sys_ParseSpeed(argv[i] + 6); if (k >= 0) menu.speed = k; }
			else if (!kwncmp(argv[i], "VIDEO=", 6)) { int k = Sys_ParseVideo(argv[i] + 6); if (k >= 0) menu.video = k; }
			else if (!kwncmp(argv[i], "MODEL=", 6)) { int k = Sys_ParseModel(argv[i] + 6); if (k >= 0) menu.model = k; }
			else if (!kwncmp(argv[i], "DISKWRITE=", 10)) menu.diskwrite = atoi(argv[i] + 10) ? 1 : 0;
			else if (!kwncmp(argv[i], "FASTTAPE=", 9)) menu.fasttape = atoi(argv[i] + 9) ? 1 : 0;
			else if (!kwncmp(argv[i], "TAPESOUND=", 10)) menu.tapesound = atoi(argv[i] + 10) ? 1 : 0;
			else if (!kwcmp(argv[i], "TAPESOUND")) menu.tapesound = 1;
			else if (!kwncmp(argv[i], "TAPE=", 5)) { strncpy(menu.tape, argv[i] + 5, SYS_PATH - 1); media_given = 1; }
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
			else if (!kwcmp(argv[i], "TALL")) tall = 1;
			else if (!kwcmp(argv[i], "WIDE")) wide = 1;
			else if (!kwcmp(argv[i], "MENU")) want_menu = 1;
			else if (!kwncmp(argv[i], "HELPAFTER=", 10)) help_after = atol(argv[i] + 10);
			else if (!kwncmp(argv[i], "P=", 2)) ;
			else if (has_ext(argv[i], ".cpr")) { strncpy(menu.cart, argv[i], SYS_PATH - 1); media_given = 1; }
			else if (has_ext(argv[i], ".dsk")) { if (!ndisk) menu.diskb[0] = 0; strncpy(ndisk++ ? menu.diskb : menu.diska, argv[i], SYS_PATH - 1); media_given = 1; }
			else if (has_ext(argv[i], ".sna")) { sna = argv[i]; media_given = 1; }
			else if (has_ext(argv[i], ".cdt")) { strncpy(menu.tape, argv[i], SYS_PATH - 1); media_given = 1; }
		}
	}
	Sys_Log("prefs '%s': sound %d hires %d skip %d refresh %d model %s video %s speed %s cart '%s' fasttape %d tapesound %d", menu.prefs, menu.sound, menu.hires, skip_max, refresh,
		Sys_ModelNames[menu.model], Sys_VideoNames[menu.video], Sys_SpeedNames[menu.speed], menu.cart, menu.fasttape, menu.tapesound);
	Sys_VideoMode = menu.video;
	if (script_on) Sys_MakeDir(snapdir);
	if (nosound) menu.sound = 0;
	if (hires) menu.hires = 1;
	if (tall) menu.tall = 1;
	if (wide) menu.wide = 1;
	global_sound = menu.sound; global_hires = menu.hires; global_joykeys = joykeys;
	global_tall = menu.tall; global_wide = menu.wide; strcpy(global_remap, remap_str); global_diskwrite = menu.diskwrite;
	global_fasttape = menu.fasttape; global_tapesound = menu.tapesound;
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
			flush_disks("menu");   /* Help: what the game saved goes to the file now */
			{ extern unsigned long Sys_AudioIrqs(void); Sys_Log("menu (audio interrupts so far %lu)", Sys_AudioIrqs()); }
			menu.skip = skip_max; menu.refresh = refresh; menu.joykeys = joykeys; menu.pad = Sys_PadType;
			strcpy(menu.remap, remap_str);
			tape_pos();
			action = Sys_Menu(&menu);
			if (strcmp(menu.remap, remap_str)) remap_parse(menu.remap);   /* key map editor / preset */
			skip_max = menu.skip; refresh = menu.refresh > 0 ? menu.refresh : 1; joykeys = menu.joykeys; Sys_PadType = menu.pad;
			Sys_VideoMode = menu.video;
			{ extern unsigned long Sys_AudioIrqs(void); Sys_Log("menu action %d (audio interrupts so far %lu)", action, Sys_AudioIrqs()); }
			if (action != SYS_MENU_POWER && action != SYS_MENU_RESET && action != SYS_MENU_CONTINUE) Sys_MenuClose();
			if (action == SYS_MENU_QUIT) break;
			if (!menu.game_prefs) { strcpy(global_remap, remap_str); global_sound = menu.sound; global_hires = menu.hires; global_joykeys = joykeys; global_tall = menu.tall; global_wide = menu.wide; global_diskwrite = menu.diskwrite; global_fasttape = menu.fasttape; global_tapesound = menu.tapesound; }
			if (action == SYS_MENU_SAVE) { int gs = menu.sound, gh = menu.hires, gt = menu.tall, gw = menu.wide; if (menu.game_prefs) { menu.sound = global_sound; menu.hires = global_hires; menu.tall = global_tall; menu.wide = global_wide; } else global_joykeys = joykeys; prefs_save(); menu.sound = gs; menu.hires = gh; menu.tall = gt; menu.wide = gw; continue; }
			if (action == SYS_MENU_SAVEGAME) { game_prefs_save(); continue; }
			if (action == SYS_MENU_FORGETGAME) { game_prefs_forget(); continue; }
			if (action == SYS_MENU_SHOT) { screenshot(); continue; }
			if (action == SYS_MENU_POWER && !power_on()) continue;
			if (action == SYS_MENU_RESET) { if (!menu.can_continue && !power_on()) continue; CPC_Reset(); }
			if (action == SYS_MENU_CONTINUE || action == SYS_MENU_RESET) { apply_disks(); apply_tape(0); }   /* disk swap, tape change or rewind */
		}
		{ int ok = open_emulation(); Sys_MenuClose(); if (!ok) { want_menu = 1; continue; } }
		menu.can_continue = 1;
		Sys_Log("running");
		CPCEmulation_Run();                 /* returns when Help is pressed (or a script QUIT) */
		Sys_Log("stopped after %lu frames", Sys_Millis() / 20);
		Sys_AudioPause();
		Sys_CloseDisplay(); display_open = 0;
		if (script_quit) {
			char name[SYS_PATH + 8]; FILE *d;
			flush_disks("exit");   /* before "done": a test runner may stop the machine then */
			sprintf(name, "%s/done", snapdir);
			d = fopen(name, "w");
			if (d) { fprintf(d, "frames=%ld\n", script_frame); perf_report(d); fclose(d); }
			perf_report(stdout);
			break;
		}
		want_menu = 1;
	}
	flush_disks("exit");
	if (audio_open) Sys_AudioClose();
	if (display_open) Sys_CloseDisplay();
	Sys_Log("exit");
	Sys_LogClose();
	return 0;
}
