/* Headless host for Arnold: renders N frames to an 8-bit chunky buffer and
 * dumps a PPM. Used to validate the core on big-endian m68k before writing
 * the AmigaOS (AGA) host. Same shape an Amiga backend will have. */
#include "../cpc/host.h"
#include "../cpc/arnold.h"
#include "../cpc/cpc.h"
#include "../cpc/render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 384
#define H 272
static unsigned char chunky[W*H*2];
extern void Palette_Set(void);
static unsigned char pal[256][3];
static int frames, maxframes = 300, depth = 8;
static const char *outname = "frame.ppm";
static GRAPHICS_BUFFER_INFO bi;
static GRAPHICS_BUFFER_COLOUR_FORMAT cf;
static SOUND_PLAYBACK_FORMAT sf = {2, 16, 22240};   /* as the Amiga host: 444 samples a frame */

void Host_HandlePrinterOutput(void) {}
BOOL Host_SetDisplay(int Type, int Width, int Height, int Depth) { return TRUE; }
GRAPHICS_BUFFER_COLOUR_FORMAT *Host_GetGraphicsBufferColourFormat(void)
{
	if (depth == 8) {
	cf.BPP = 8;
	cf.Red.BPP = 3; cf.Red.Mask = 0xe0; cf.Red.Shift = 5;
	cf.Green.BPP = 3; cf.Green.Mask = 0x1c; cf.Green.Shift = 2;
	cf.Blue.BPP = 2; cf.Blue.Mask = 0x03; cf.Blue.Shift = 0;
	} else {
	cf.BPP = 16;
	cf.Red.BPP = 5; cf.Red.Mask = 0xf800; cf.Red.Shift = 11;
	cf.Green.BPP = 6; cf.Green.Mask = 0x07e0; cf.Green.Shift = 5;
	cf.Blue.BPP = 5; cf.Blue.Mask = 0x001f; cf.Blue.Shift = 0;
	}
	return &cf;
}
GRAPHICS_BUFFER_INFO *Host_GetGraphicsBufferInfo(void)
{
	bi.pSurface = chunky; bi.Width = W; bi.Height = H; bi.Pitch = W * (depth / 8);
	return &bi;
}
BOOL Host_LockGraphicsBuffer(void) { return TRUE; }
void Host_UnlockGraphicsBuffer(void) {}
static void framesum(void);
static void undrawn(void);
void Host_SwapGraphicsBuffers(void) { frames++; if (depth == 8) Palette_Set(); if (getenv("FRAMESUM")) framesum(); if (getenv("UNDRAWN")) undrawn(); }
void Host_SetPaletteEntry(int i, unsigned char r, unsigned char g, unsigned char b)
{
	pal[i & 255][0] = r; pal[i & 255][1] = g; pal[i & 255][2] = b;
}
/* Sound (argv[7] = WAV file): the core's blocks, through the Amiga host's
 * conversion (audio_conv.h), as 8-bit stereo at the Amiga playback rate. */
#include "../amiga/audio_conv.h"
static FILE *wav; static unsigned long wav_bytes;
/* TAPESOUND=1 (25 Oct 2026): mix the tape signal like the Amiga host's Tape sound option (weak: the ref core for headless_check has none) */
extern int TapeAudio_On __attribute__((weak));
extern void TapeAudio_Mix(unsigned short *, int) __attribute__((weak));
static int ts_env;
static unsigned short abuf[8192]; static unsigned long alen;
void Host_WriteDataToSoundBuffer(unsigned char *p, unsigned long l) {}
BOOL Host_AudioPlaybackPossible(void) { return wav != NULL; }
SOUND_PLAYBACK_FORMAT *Host_GetSoundPlaybackFormat(void) { return &sf; }
BOOL Host_LockAudioBuffer(unsigned char **a, unsigned long *b, unsigned char **c, unsigned long *d, int e)
{
	if (!wav || e > (int)sizeof abuf) return FALSE;
	*a = (unsigned char *)abuf; *b = e; *c = NULL; *d = 0; alen = e; return TRUE;
}
void Host_UnLockAudioBuffer(void)
{
	signed char l[4096], r[4096]; unsigned char out[8192]; int i, n = (int)(alen / 4);
	if (ts_env && &TapeAudio_Mix) TapeAudio_Mix(abuf, n);
	Audio_SplitTo8(abuf, l, r, n);
	for (i = 0; i < n; i++) { out[2 * i] = (unsigned char)(l[i] + 128); out[2 * i + 1] = (unsigned char)(r[i] + 128); }
	fwrite(out, 1, 2 * n, wav); wav_bytes += 2 * n;
	/* RAWIDX=file (soundq, 21 Oct 2026): the core's mixed left/right bytes
	 * before the 16-bit lookup (audioevent.c, 4 bytes a sample), to check
	 * the lookup offline (tools/soundq) */
	{ static FILE *raw; static int tried; extern unsigned char *pAudioBuffer2;
	  if (!tried) { tried = 1; if (getenv("RAWIDX")) raw = fopen(getenv("RAWIDX"), "wb"); }
	  static FILE *raw16; static int tried16;   /* RAW16=file: the core's 16-bit values, before the 8-bit conversion */
	  if (!tried16) { tried16 = 1; if (getenv("RAW16")) raw16 = fopen(getenv("RAW16"), "wb"); }
	  if (raw16) fwrite(abuf, 2, 2 * n, raw16);
	  static FILE *rawt;
	  if (raw && !rawt) { char tn[600]; snprintf(tn, sizeof tn, "%s.t", getenv("RAWIDX")); rawt = fopen(tn, "w"); }
	  if (rawt) fprintf(rawt, "%lu\n", CPC_GetNopCount());   /* the block's end (NOP time) */
	  if (raw && pAudioBuffer2) for (i = 0; i < n; i++) { fputc(pAudioBuffer2[4 * i], raw); fputc(pAudioBuffer2[4 * i + 1], raw); } }
}
static void wav_header(void)
{
	unsigned char h[44] = "RIFF....WAVEfmt ";
	unsigned long v; int i;
#define PUT32(o, x) for (v = (x), i = 0; i < 4; i++) h[(o) + i] = (unsigned char)(v >> (8 * i))
#define PUT16(o, x) for (v = (x), i = 0; i < 2; i++) h[(o) + i] = (unsigned char)(v >> (8 * i))
	PUT32(4, 36 + wav_bytes); PUT32(16, 16); PUT16(20, 1); PUT16(22, 2); PUT32(24, sf.Frequency);
	PUT32(28, sf.Frequency * 2); PUT16(32, 2); PUT16(34, 8);
	memcpy(h + 36, "data", 4); PUT32(40, wav_bytes);
	fseek(wav, 0, SEEK_SET); fwrite(h, 1, 44, wav);
}
void Host_Throttle(void) { if (wav) CPC_UpdateAudio(); }
void Host_DoDriveLEDIndicator(int Drive, BOOL State) {}
void Host_SetDirectory(char *d) {}
unsigned long Host_GetCurrentTimeInMilliseconds(void) { return (unsigned long)(clock() / (CLOCKS_PER_SEC / 1000)); }
/* Optional key script (argv[5], same format as the Amiga host's KEYS=), so
 * pixel-exact checks can cover gameplay, not just attract modes. */
#include "../amiga/keyscript.h"
static int script_on;
static long script_frame;
static void snap(long frame);
static void script_setkey(int key, int down) { if (down) CPC_SetKey(key); else CPC_ClearKey(key); }
/* argv[6] = -1: the Amiga host's frame-skip hook with a pseudo-random pattern
 * (runs of 0-3 skipped frames); maxframes then counts emulated frames and the
 * last one is always drawn, so the result can be compared with drawing all. */
static int hook_mode;
extern void CPC_PowerCycleMemory(void) __attribute__((weak));   /* absent in older cores */
extern void Palette_Reset(void);
static long emu_frames;
static unsigned long lcg = 12345;
extern int (*CPC_SkipNextFrameHook)(void) __attribute__((weak));   /* absent in older cores */
static int test_skip_hook(void)
{
	/* decides frame emu_frames + 1 (0-based); emu_frames frames have ended */
	if (emu_frames + 1 >= maxframes - 1) return 0;
	if (hook_mode == 2) return 1;           /* argv[6] = -2: skip every frame but the last (skipped-frame cost) */
	lcg = lcg * 1103515245UL + 12345UL;
	return ((lcg >> 16) & 3) != 0;          /* skip 3 in 4 on average */
}
/* Tapes (24 Oct 2026): TAPE=<file.cdt> inserts a tape after the reset (as the
 * Amiga host's power_on()). TAPEFAST=1 skips drawing frames while the motor
 * runs and the tape has blocks left, like the Amiga host's Fast tape (one frame
 * in 25 drawn; a frame the script will SNAP is drawn). Motor changes are
 * logged as "tape: frame N motor on|off block i/n". The accessors are weak so
 * headless_check can build this host against an older core. */
#include "../cpc/tzx.h"
extern int CPC_GetTapeMotor(void) __attribute__((weak));
extern int TapeImage_IsPlayable(void) __attribute__((weak));
static int tape_in, tape_motor, tape_fast_count;
static long tape_frame;
static void tape_tick(void)
{
	int m;
	tape_frame++;
	if (ts_env && &TapeAudio_On) TapeAudio_On = tape_in;
	if (!tape_in || !&CPC_GetTapeMotor) return;
	m = CPC_GetTapeMotor();
	if (m != tape_motor) {
		tape_motor = m;
		printf("tape: frame %ld motor %s block %d/%d\n", tape_frame, m ? "on" : "off", TapeImage_GetCurrentBlock(), TapeImage_GetNumBlocks());
	}
}
static int tape_skip_hook(void)
{
	if (script_on && KeyScript_SnapAt(script_frame + 1)) return 0;
	if (!(CPC_GetTapeMotor() && TapeImage_IsPlayable())) { tape_fast_count = 0; return 0; }
	return (tape_fast_count++ % 25) != 0;
}
static void insert_tape(const char *var)
{
	const char *n = getenv(var); unsigned char *d; unsigned long l; int r;
	if (!n || !*n) return;
	if (!Host_LoadFile(n, &d, &l)) { printf("tape: can't load %s\n", n); return; }
	r = TapeImage_Insert(d, l);
	free(d);   /* the core keeps its own copy */
	tape_in = r == ARNOLD_STATUS_OK;
	printf("tape: %s (%lu bytes, %d blocks) %s\n", n, l, TapeImage_GetNumBlocks(), tape_in ? "inserted" : "NOT RECOGNISED");
	if (tape_in && getenv("TAPEFAST") && atoi(getenv("TAPEFAST")) && &TapeImage_IsPlayable && &CPC_SkipNextFrameHook) CPC_SkipNextFrameHook = tape_skip_hook;
}
#ifdef TRACE_HW
extern int Trace_On;
#endif
/* Auto-start tapes (1.5): AUTOTAPE=1 with TAPE= types the load command as the
 * Amiga host does (amiga/tapeauto.c, included here so the build lists need no
 * change; absent in an older tree, then AUTOTAPE does nothing) */
#if defined(__has_include)
#if __has_include("../amiga/tapeauto.c")
#include "../amiga/tapeauto.c"
#define HAVE_TAPEAUTO
#endif
#endif
#ifdef HAVE_TAPEAUTO
#define TA_GET(v) TapeAuto_GetState(v)
#define TA_SET(v) TapeAuto_SetState(v)
#define TA_STOP() TapeAuto_Stop()
#else
#define TA_GET(v) ((v)[0] = (v)[1] = 0)
#define TA_SET(v) ((void)0)
#define TA_STOP() ((void)0)
#endif
/* Save states (28 Oct 2026, cpc/savestate.c). Environment:
 *   SS_SAVE=n1,n2,...  SS_FILE=<prefix>: after frame n (counted frame ends), the
 *       state goes to <prefix>.<n>.sna (the run stops at that frame end and goes on)
 *   SS_LOAD=<file>: load it before the emulation starts (exit 2 if refused)
 *   SS_ROUNDTRIP=<file>: right after SS_LOAD, save again to <file> (then exit)
 * The host's own counters, the key script position and the sound DC filter
 * travel in the state's host data, so a loaded run goes on as the saving one did.
 * Weak: headless_check builds this host against older cores too. */
#if defined(__has_include)
#if __has_include("../cpc/savestate.h")
#include "../cpc/savestate.h"
#define SS_HAVE_HEADER
#endif
#endif
#ifndef SS_HAVE_HEADER   /* a core from before save states (headless_check's REF build) */
typedef struct { const char *cart_name, *disk_name[2], *tape_name; const unsigned char *host; unsigned long hostlen; int embed_disk[2]; } SAVESTATE_INFO;
typedef struct { const unsigned char *host; unsigned long hostlen; int disk[2], tape; char cart_name[64], disk_name[2][64], tape_name[64]; } SAVESTATE_LOADED;
#define SAVESTATE_REFUSED 0
#define SAVESTATE_PLAIN 2
#endif
extern int SaveState_Save(unsigned char **, unsigned long *, const SAVESTATE_INFO *, char *, int) __attribute__((weak));
extern int SaveState_Load(const unsigned char *, unsigned long, char *, int, SAVESTATE_LOADED *) __attribute__((weak));
static long ss_fe;                 /* frame ends so far */
static int ss_due, ss_done;
static long ss_points[64]; static int ss_npoints;
static void ss_put8(unsigned char *p, long v) { int i; unsigned long long u = (unsigned long long)(long long)v; for (i = 7; i >= 0; i--) { p[i] = (unsigned char)u; u >>= 8; } }
static long ss_get8(const unsigned char *p) { int i; unsigned long long u = 0; for (i = 0; i < 8; i++) u = (u << 8) | p[i]; return (long)(long long)u; }
/* The host data both hosts write (8-byte big-endian values), so a state from the
 * Amiga goes on here and one from here goes on on the Amiga: "ARNH", the key
 * script's frame, the sound DC filter (x l/r, y l/r, on), the host (1 headless,
 * 2 Amiga), then that host's own: here the frame counters and the tape's. */
/* 1.5: two more, the tape auto-start (kind, frame); a 1.4 state has 15 */
#define SS_HOSTN 17
#define SS_HOSTN14 15
#define SS_HOSTMAGIC 0x41524E48L
static void ss_hostblob(unsigned char *b, unsigned long len, int get)
{
	long v[SS_HOSTN];
	int i, n = get ? (int)(len / 8) : SS_HOSTN;
	if (!get) {
		v[0] = SS_HOSTMAGIC; v[1] = script_frame;
#if !defined(NO_SNDLUT_FIX) && !defined(NO_SNDUNIPOLAR)
		v[2] = Audio_DcX[0]; v[3] = Audio_DcX[1]; v[4] = Audio_DcY[0]; v[5] = Audio_DcY[1]; v[6] = Audio_DcOn;
#else
		v[2] = v[3] = v[4] = v[5] = v[6] = 0;
#endif
		v[7] = 1;
		v[8] = frames; v[9] = emu_frames; v[10] = (long)lcg; v[11] = tape_frame; v[12] = tape_motor; v[13] = tape_fast_count; v[14] = ss_fe;
		TA_GET(v + 15);
		for (i = 0; i < SS_HOSTN; i++) ss_put8(b + 8 * i, v[i]);
		return;
	}
	if (n < 8 || ss_get8(b) != SS_HOSTMAGIC) return;
	for (i = 0; i < n && i < SS_HOSTN; i++) v[i] = ss_get8(b + 8 * i);
	script_frame = v[1];
#if !defined(NO_SNDLUT_FIX) && !defined(NO_SNDUNIPOLAR)
	Audio_DcX[0] = v[2]; Audio_DcX[1] = v[3]; Audio_DcY[0] = v[4]; Audio_DcY[1] = v[5]; Audio_DcOn = (int)v[6];
#endif
	if (v[7] == 1 && n >= SS_HOSTN) TA_SET(v + 15); else TA_STOP();
	if (v[7] == 1 && n >= SS_HOSTN14) {
		frames = (int)v[8]; emu_frames = v[9]; lcg = (unsigned long)v[10]; tape_frame = v[11];
		tape_motor = (int)v[12]; tape_fast_count = (int)v[13]; ss_fe = v[14];
	} else {
		/* another host's state: its frame ends are the script's frames */
		frames = (int)v[1]; emu_frames = ss_fe = tape_frame = v[1];
	}
	if (script_on) KeyScript_Seek(script_frame);
}
static int ss_save_to(const char *name)
{
	unsigned char blob[8 * SS_HOSTN], *d; unsigned long l;
	char msg[300];
	SAVESTATE_INFO info;
	FILE *f;
	memset(&info, 0, sizeof info);
	info.cart_name = getenv("SS_CARTNAME"); info.disk_name[0] = getenv("DISKA"); info.disk_name[1] = getenv("DISKB"); info.tape_name = getenv("TAPE");
	ss_hostblob(blob, sizeof blob, 0);
	info.host = blob; info.hostlen = sizeof blob;
	if (!&SaveState_Save || !SaveState_Save(&d, &l, &info, msg, sizeof msg)) { printf("state: save at frame %ld FAILED: %s\n", ss_fe, &SaveState_Save ? msg : "no save states in this core"); return 0; }
	f = fopen(name, "wb");
	if (!f || fwrite(d, 1, l, f) != l) { printf("state: can't write %s\n", name); if (f) fclose(f); free(d); return 0; }
	fclose(f); free(d);
	printf("state: saved frame %ld to %s (%lu bytes)\n", ss_fe, name, l);
	return 1;
}
static void ss_setup(void)
{
	const char *p = getenv("SS_SAVE");
	while (p && *p && ss_npoints < 64) {
		ss_points[ss_npoints++] = atol(p);
		p = strchr(p, ',');
		if (p) p++;
	}
}
static int ss_due_at(long fe)
{
	int i;
	for (i = 0; i < ss_npoints; i++) if (ss_points[i] == fe) return 1;
	return 0;
}
/* Fast-forward (1.5): FFWD=a-b runs frame ends a to b as the Amiga host's held
 * fast-forward key does (only 1 frame in 8 drawn; a frame the script will SNAP is
 * drawn), to show the emulation is unchanged: the snaps, the final frame and the
 * WAV must equal a run without FFWD. Other skip hooks (TAPEFAST, argv[6] -1) run
 * outside the range. */
static long ffwd_a = -1, ffwd_b = -1;
static int (*ffwd_prev)(void);
static long ffwd_skipped;
static int ffwd_hook(void)
{
	if (ss_fe >= ffwd_a && ss_fe <= ffwd_b) {
		if (script_on && KeyScript_SnapAt(script_frame + 1)) return 0;
		if (ss_fe % 8) { ffwd_skipped++; return 1; }
		return 0;
	}
	return ffwd_prev ? ffwd_prev() : 0;
}
static void ffwd_setup(void)
{
	const char *f = getenv("FFWD"), *d;
	if (!f || !&CPC_SkipNextFrameHook) return;
	ffwd_a = atol(f); d = strchr(f, '-'); ffwd_b = d ? atol(d + 1) : ffwd_a;
	ffwd_prev = CPC_SkipNextFrameHook; CPC_SkipNextFrameHook = ffwd_hook;
	printf("ffwd: frame ends %ld-%ld\n", ffwd_a, ffwd_b);
}
BOOL Host_ProcessSystemEvents(void)
{
	int ss_quit = 0;
	ss_fe++;
	tape_tick();
#ifdef TRACE_HW
	{ static int fr; const char *a = getenv("TRACE_AT"); int at = a ? atoi(a) : -1; fr++;
	  Trace_On = (fr == at) || (getenv("TRACE_FROM") && fr >= atoi(getenv("TRACE_FROM"))); if (fr == 600 && getenv("RAMDUMP")) { extern unsigned char *Z80MemoryBase; FILE *mf = fopen(getenv("RAMDUMP"), "wb"); if (mf) { fwrite(Z80MemoryBase, 1, 65536, mf); fclose(mf); } }
	  if (Trace_On) { extern void Trace_Regs(void); fprintf(stderr, "=== frame %d ===\n", fr); Trace_Regs(); } }
#endif
	/* PCSAMPLE=n: the Z80's PC, SP, AF, HL, IFF1 and IM at each frame end, from
	 * frame n for 40 frames (where a game is stuck; 18 Oct 2026, Puzzle Bobble) */
	{ static long pf; const char *ps = getenv("PCSAMPLE"); pf++;
	  if (ps && pf >= atol(ps) && pf < atol(ps) + 40) {
		extern int Z80_GetReg(int);
		fprintf(stderr, "frame %ld PC %04x SP %04x AF %04x HL %04x IFF1 %d IM %d\n", pf, Z80_GetReg(0) & 0xffff,
			Z80_GetReg(1) & 0xffff, Z80_GetReg(6) & 0xffff, Z80_GetReg(9) & 0xffff, Z80_GetReg(10), Z80_GetReg(12)); } }
	if (script_on) {
		int ks = KeyScript_Frame(script_frame, script_setkey);
		if (ks & KS_SNAP) snap(script_frame);   /* SNAP: dump the last drawn frame as <out>.<frame>.ppm */
		script_frame++;
		if (ks & KS_QUIT) ss_quit = 1;
	}
#ifdef HAVE_TAPEAUTO
	TapeAuto_Frame(script_setkey);
#endif
	if (!ss_quit) ss_quit = hook_mode ? ++emu_frames >= maxframes : frames >= maxframes;
	if (ss_due_at(ss_fe)) { ss_due = 1; ss_done = ss_quit; return TRUE; }   /* save states: stop here, save, go on */
	return ss_quit;
}

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
	if (!p || fread(p, 1, s, f) != (size_t)s) { fclose(f); return FALSE; }
	fclose(f); *pp = p; *pl = s; return TRUE;
}
BOOL Host_SaveFile(const char *n, unsigned char *p, unsigned long l)
{
	FILE *f = fopen(n, "wb"); if (!f) return FALSE;
	fwrite(p, 1, l, f); fclose(f); return TRUE;
}

static unsigned long dump_to(const char *name)
{
	FILE *f = fopen(name, "wb"); int i; unsigned long sum = 0;
	if (!f) return 0;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (i = 0; i < W*H; i++) {
		if (depth == 8) {
			unsigned char c = chunky[i];
			/* AMIGACOL0=1: index 0 shows the border colour, as Amiga colour 0 does */
			{ extern int Render_PaletteIndexBorder; static int a0 = -1; if (a0 < 0) a0 = getenv("AMIGACOL0") != NULL;
			  if (a0 && c == 0) c = (unsigned char)Render_PaletteIndexBorder; }
			fwrite(pal[c], 1, 3, f);
			/* RGB, not the palette index (20 Oct 2026): index numbers may change
			 * (palette freeing, PALGC_TOP) while the picture stays the same */
			sum = ((sum * 31 + pal[c][0]) * 31 + pal[c][1]) * 31 + pal[c][2];
		} else {
			unsigned short v = ((unsigned short *)chunky)[i]; unsigned char rgb[3];
			rgb[0] = (v >> 11) << 3; rgb[1] = ((v >> 5) & 63) << 2; rgb[2] = (v & 31) << 3;
			fwrite(rgb, 1, 3, f);
			sum = sum * 31 + v;
		}
	}
	fclose(f);
	return sum & 0xffffffffUL;
}
/* UNDRAWN=1 (pinball, 20 Oct 2026): rows of the frame the core did not draw
 * (still holding the marker byte 0xFE written after the previous frame) */
static void undrawn(void)
{
	int y, x, n; char buf[2048]; int len = 0;
	if (depth != 8) return;
	for (y = 0; y < H; y++) {
		for (n = 0, x = 0; x < W; x++) n += chunky[y * W + x] == 0xFE;
		if (n && len < 1900) len += sprintf(buf + len, " %d(%d)", y, n);
	}
	if (len) printf("undrawn %d:%s\n", frames, buf);
	memset(chunky, 0xFE, W * H);
}
/* FRAMESUM=1: the RGB checksum of every drawn frame (pinball, 20 Oct 2026) */
static void framesum(void)
{
	int i; unsigned long sum = 0;
	for (i = 0; i < W*H; i++) {
		if (depth == 8) { unsigned char c = chunky[i]; sum = ((sum * 31 + pal[c][0]) * 31 + pal[c][1]) * 31 + pal[c][2]; }
		else sum = sum * 31 + ((unsigned short *)chunky)[i];
	}
	printf("framesum %d %08lx\n", frames, sum & 0xffffffffUL);
}
static void dump(void) { unsigned long sum = dump_to(outname); printf("frames=%d checksum=%08lx\n", frames, sum); }
static void snap(long frame)
{
	char name[1024]; unsigned long sum;
	snprintf(name, sizeof name, "%s.%ld.ppm", outname, frame);
	sum = dump_to(name);
	printf("snap frame=%ld checksum=%08lx\n", frame, sum);
}

/* Disks (environment, so the argv layout stays as it was): DISKA= / DISKB=
 * image paths, inserted after the reset as the Amiga host's power_on() does.
 * DISKOUT= / DISKOUTB=: write that drive's image back out (Arnold's internal
 * EXTENDED form) at the end if the emulated machine wrote to it. */
#include "../cpc/diskimage/diskimg.h"
#include "../cpc/diskimage/dskwrite.h"
static unsigned char *wb_img[2]; static unsigned long wb_len[2];
static void insert_disk(int drive, const char *var)
{
	const char *n = getenv(var); unsigned char *d; unsigned long l; int r;
	if (!n || !*n) return;
	if (!Host_LoadFile(n, &d, &l)) { printf("disk %c: can't load %s\n", 'A' + drive, n); return; }
	r = DiskImage_InsertDisk(drive, d, l);
	printf("disk %c: %s (%lu bytes) %s\n", 'A' + drive, n, l, r == ARNOLD_STATUS_OK ? "inserted" : "NOT RECOGNISED");
	if (getenv("DISKWB") && atoi(getenv("DISKWB")) && r == ARNOLD_STATUS_OK) { wb_img[drive] = d; wb_len[drive] = l; return; }
	free(d);   /* the core keeps its own copy */
}
/* DISKWB=1: at the end, write each changed drive back into its DISKA/DISKB file
 * the way the Amiga host does (cpc/diskimage/dskwrite.c: same format, patched
 * sectors, .tmp + rename, .bak the first time). */
static void writeback_disk(int drive, const char *var)
{
	const char *n = getenv(var); char msg[DSKWRITE_MSG]; int r;
	if (!n || !*n || !wb_img[drive]) return;
	r = DskWrite_Flush(drive, n, &wb_img[drive], &wb_len[drive], msg);
	printf("disk %c write-back: %s\n", 'A' + drive, r == 0 ? "not changed" : msg);
}
#include <unistd.h>
int Host_FileWritable(const char *name) { return access(name, W_OK) == 0; }
/* Names on a volume that cuts them (tools/dskwb_tests.py): DISKWB_TRUNC=30 makes
 * the host behave like OFS/FFS of an old Kickstart: two names are one file if
 * they agree up to 30 characters of the file part, and a longer name is "not kept". */
#include <sys/stat.h>
static int trunc_len(void) { const char *t = getenv("DISKWB_TRUNC"); return t ? atoi(t) : 0; }
static int cut_eq(const char *a, const char *b, int n)
{
	const char *fa = strrchr(a, '/'), *fb = strrchr(b, '/');
	size_t da = fa ? (size_t)(fa - a) : 0, db = fb ? (size_t)(fb - b) : 0;
	fa = fa ? fa + 1 : a; fb = fb ? fb + 1 : b;
	return da == db && !strncmp(a, b, da) && !strncmp(fa, fb, n);
}
int Host_SameFile(const char *a, const char *b)
{
	struct stat sa, sb; int n = trunc_len();
	if (n && cut_eq(a, b, n)) return 1;   /* simulated: one stored name */
	return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}
int Host_NameKept(const char *name)
{
	const char *f = strrchr(name, '/'); int n = trunc_len();
	f = f ? f + 1 : name;
	if (n && (int)strlen(f) > n) return 0;
	return 1;
}
/* DISKDUMP=file: list every sector the core holds for drive A after the insert
 * (track side index C H R N ST1 ST2 FNV-1a of the 128<<N bytes the FDC gets),
 * for tools/dsk_tests.py to compare with its own reading of the image. */
#pragma push_macro("H")
#undef H
static void dump_disk(int drive, const char *var)
{
	static char buf[32768];
	const char *n = getenv(var); FILE *f; int t, s, i;
	if (!n || !*n || !(f = fopen(n, "w"))) return;
	for (t = 0; t < 85; t++)
		for (s = 0; s < 2; s++) {
			int spt = DiskImage_GetSectorsPerTrack(drive, t, s);
			for (i = 0; i < spt; i++) {
				CHRN c; unsigned long h = 2166136261UL; int k, len;
				DiskImage_GetID(drive, t, s, i, &c);
				len = 128 << (c.N & 7);
				memset(buf, 0, sizeof buf);
				DiskImage_GetSector(drive, t, s, i, buf);
				for (k = 0; k < len; k++) h = ((h ^ (unsigned char)buf[k]) * 16777619UL) & 0xffffffffUL;
				fprintf(f, "%d %d %d %02X %02X %02X %02X %02X %02X %08lX\n", t, s, i, c.C, c.H, c.R, c.N, c.ST1, c.ST2, h);
			}
		}
	fclose(f);
}
#pragma pop_macro("H")
static void save_disk(int drive, const char *var)
{
	const char *n = getenv(var); unsigned long l; unsigned char *d;
	if (!n || !*n) return;
	if (!DiskImage_IsImageDirty(drive)) { printf("disk %c: not modified\n", 'A' + drive); return; }
	l = DiskImage_CalculateOutputSize(drive);
	d = malloc(l);
	if (!d) return;
	DiskImage_GenerateOutputData(d, drive);
	Host_SaveFile(n, d, l);
	printf("disk %c: modified, %lu bytes written to %s\n", 'A' + drive, l, n);
	free(d);
}

int main(int argc, char **argv)
{
	unsigned char *cart; unsigned long cartlen; const char *cartname = "../roms/cpcplus/system.cpr";
	if (argc > 1) maxframes = atoi(argv[1]);
	if (argc > 2) outname = argv[2];
	if (argc > 3) cartname = argv[3];
	if (argc > 4) depth = atoi(argv[4]);
	if (argc > 5 && KeyScript_Load(argv[5]) >= 0) script_on = 1;
	if (argc > 6 && atoi(argv[6]) < 0 && &CPC_SkipNextFrameHook) { hook_mode = atoi(argv[6]) == -2 ? 2 : 1; CPC_SkipNextFrameHook = test_skip_hook; }
	else if (argc > 6) CPC_SetFrameSkip(atoi(argv[6]));   /* render 1 frame in N+1 (cost of skipped frames) */
	if (!CPCEmulation_CheckEndianness()) { printf("ENDIANNESS CHECK FAILED\n"); return 1; }
	CPC_Initialise();
	CPCEmulation_InitialiseDefaultSetup();
	if (getenv("HW") && strcmp(getenv("HW"), "PLUS") && strcmp(getenv("HW"), "GX4000")) {
		/* HW=464|664|6128: a plain CPC with the system ROMs from ROMDIR (default
		 * ../roms), as the Amiga host's Model setting does it (19 Oct 2026); the
		 * cartridge argument is ignored */
		const char *hw = getenv("HW"), *rd = getenv("ROMDIR") ? getenv("ROMDIR") : "../roms";
		char n[512]; unsigned char *os, *basic, *dos = NULL; unsigned long l;
		const char *d = !strcmp(hw, "464") ? "cpc464e" : !strcmp(hw, "664") ? "cpc664e" : "cpc6128e";
		sprintf(n, "%s/%s/os.rom", rd, d); if (!Host_LoadFile(n, &os, &l)) { printf("no %s\n", n); return 1; }
		sprintf(n, "%s/%s/basic.rom", rd, d); if (!Host_LoadFile(n, &basic, &l)) { printf("no %s\n", n); return 1; }
		if (strcmp(hw, "464")) { sprintf(n, "%s/amsdose/amsdos.rom", rd); if (!Host_LoadFile(n, &dos, &l)) { printf("no %s\n", n); return 1; } }
		CPC_SetOSRom(os); CPC_SetBASICRom(basic); CPC_SetDOSRom(dos);
		CPC_SetHardware(CPC_HW_CPC);
		if (dos) Amstrad_DiscInterface_Install(); else Amstrad_DiscInterface_DeInstall();
		if (!strcmp(hw, "6128")) Amstrad_RamExpansion_Install(); else Amstrad_RamExpansion_DeInstall();
		CPC_Reset();
	} else {
	if (!Host_LoadFile(cartname, &cart, &cartlen)) { printf("no cart\n"); return 1; }
	CPC_SetHardware(CPC_HW_CPCPLUS);
	Cartridge_Insert(cart, cartlen);
	Amstrad_DiscInterface_Install();
	Amstrad_RamExpansion_Install();
	/* HW=GX4000 (save state tests, 28 Oct 2026): the console, as the Amiga host's model
	 * (no disc interface, 64 KB) */
	if (getenv("HW") && !strcmp(getenv("HW"), "GX4000")) { Amstrad_DiscInterface_DeInstall(); Amstrad_RamExpansion_DeInstall(); }
	CPC_Reset();
	}
	insert_disk(0, "DISKA");
	insert_disk(1, "DISKB");
	insert_tape("TAPE");
#ifdef HAVE_TAPEAUTO
	if (tape_in && getenv("AUTOTAPE") && atoi(getenv("AUTOTAPE"))) {
		const char *hw = getenv("HW");
		int k = !hw || !strcmp(hw, "PLUS") ? TAPEAUTO_PLUS : !strcmp(hw, "464") ? TAPEAUTO_464 : !strcmp(hw, "GX4000") ? TAPEAUTO_OFF : TAPEAUTO_DISC;
		TapeAuto_Start(k);
		printf("tape: auto-start (%s)\n", k ==TAPEAUTO_PLUS ? "f1, |TAPE, RUN\"" : k == TAPEAUTO_464 ? "RUN\"" : k ? "|TAPE, RUN\"" : "off");
	}
#endif
	ffwd_setup();
	ts_env = getenv("TAPESOUND") && atoi(getenv("TAPESOUND"));
	dump_disk(0, "DISKDUMP");
	Render_SetDisplayWindowed();
	if (argc > 7 && argv[7][0] != '-') { wav = fopen(argv[7], "wb"); if (wav) { fseek(wav, 44, SEEK_SET); CPC_SetAudioActive(TRUE); } }
	ss_setup();
	if (getenv("SS_LOAD")) {
		unsigned char *d; unsigned long l;
		SAVESTATE_LOADED ld;
		char msg[300]; int r;
		if (!Host_LoadFile(getenv("SS_LOAD"), &d, &l)) { d = NULL; l = 0; }   /* a missing file: refused like any other */
		r = &SaveState_Load ? SaveState_Load(d, l, msg, sizeof msg, &ld) : 0;
		if (r == SAVESTATE_REFUSED) { printf("state: REFUSED: %s\n", &SaveState_Load ? msg : "no save states in this core"); free(d); if (!getenv("SS_GOON")) return 2; }
		else if (r == SAVESTATE_PLAIN) printf("state: plain snapshot loaded: %s\n", msg);
		else {
			ss_hostblob((unsigned char *)ld.host, ld.hostlen, 1);
			printf("state: loaded %s at frame %ld (exact)%s%s%s\n", getenv("SS_LOAD"), ss_fe, ld.disk[0] == 2 ? ", drive A: the state's disk" : ld.disk[0] == 1 ? ", drive A emptied" : "",
				ld.disk[1] == 2 ? ", drive B: the state's disk" : ld.disk[1] == 1 ? ", drive B emptied" : "", ld.tape ? ", tape taken out" : "");
		}
		if (r != SAVESTATE_REFUSED) free(d);
		if (getenv("SS_ROUNDTRIP")) return ss_save_to(getenv("SS_ROUNDTRIP")) ? 0 : 3;
	}
	for (;;) {
		CPCEmulation_Run();
		if (!ss_due) break;
		ss_due = 0;
		{ char n[1024]; snprintf(n, sizeof n, "%s.%ld.sna", getenv("SS_FILE") ? getenv("SS_FILE") : "state", ss_fe); ss_save_to(n); }
		if (ss_done) break;
	}
	/* POWERON=n:cart.cpr: after n frames, a second Power ON with another cartridge, as
	 * the Amiga host does it from the menu (emulation stopped, cart in, Plus hardware,
	 * reset), then maxframes more frames. To compare a game started after another one
	 * with the same game started fresh (18 Oct 2026). */
	if (getenv("POWERON")) {
		const char *po = getenv("POWERON"), *c2 = strchr(po, ':');
		unsigned char *cart2; unsigned long len2;
		if (c2 && Host_LoadFile(c2 + 1, &cart2, &len2)) {
			CPC_SetHardware(CPC_HW_CPCPLUS);
			Cartridge_Insert(cart2, len2);
			Amstrad_DiscInterface_Install();
			Amstrad_RamExpansion_Install();
			if (!getenv("POWERON_NOCLEAR") && &CPC_PowerCycleMemory) { CPC_PowerCycleMemory(); Palette_Reset(); }
			CPC_Reset();
			frames = 0; script_frame = 0;
			if (script_on) KeyScript_Load(getenv("POWERON_KEYS") ? getenv("POWERON_KEYS") : argv[5]);
			if (getenv("POWERON_FRAMES")) maxframes = atoi(getenv("POWERON_FRAMES"));
			CPCEmulation_Run();
		}
	}
	if (wav) { wav_header(); fclose(wav); }
#ifdef STATS_E
	{ extern unsigned long StatsE_Prepare, StatsE_Invalidate, StatsE_Runs, StatsE_RunCycles, StatsE_Events, StatsE_Calls;
	  fprintf(stderr, "per frame: calls %lu prepare %lu invalidated-by-hook %lu runs %lu run-cycles %lu events %lu\n", StatsE_Calls/frames, StatsE_Prepare/frames, StatsE_Invalidate/frames, StatsE_Runs/frames, StatsE_RunCycles/frames, StatsE_Events/frames); }
#endif
	if (tape_in) printf("tape: end frame %ld motor %s block %d/%d\n", tape_frame, &CPC_GetTapeMotor && CPC_GetTapeMotor() ? "on" : "off", TapeImage_GetCurrentBlock(), TapeImage_GetNumBlocks());
	if (ffwd_a >= 0) printf("ffwd: %ld frames not drawn\n", ffwd_skipped);
	dump();
	save_disk(0, "DISKOUT");
	save_disk(1, "DISKOUTB");
	writeback_disk(0, "DISKA");
	writeback_disk(1, "DISKB");
	if (getenv("RAMOUT")) { extern unsigned char *Z80MemoryBase; Host_SaveFile(getenv("RAMOUT"), Z80MemoryBase, 65536); }   /* base 64K RAM at the end */
	return 0;
}
BOOL LoadFile(const char *n, unsigned char **pp, unsigned long *pl) { return Host_LoadFile(n, pp, pl); }
