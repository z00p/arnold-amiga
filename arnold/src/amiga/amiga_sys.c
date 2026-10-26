/* Arnold on AmigaOS: screen, c2p, palette, keyboard and joystick.
 * AmigaOS 3.x, AGA. Built with bebbo's m68k-amigaos-gcc, -noixemul. */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/ports.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/gfxbase.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <graphics/view.h>
#include <graphics/copper.h>
#include <hardware/custom.h>
#include <hardware/cia.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "amiga_sys.h"
#include <stdarg.h>

unsigned long __stack = 256 * 1024;   /* libnix: enlarge the stack at start */
static const char ver[] __attribute__((used)) = "$VER: Arnold 1.2 (26.10.2026) Amiga port, GPL-2.0";

/* ---- diagnostic log ---- */
static FILE *logf;
void Sys_LogOpen(void)
{
	logf = fopen("PROGDIR:Arnold.log", "w");
	if (!logf) logf = fopen("T:Arnold.log", "w");
}
void Sys_Log(const char *fmt, ...)
{
	va_list ap;
	if (!logf) return;
	va_start(ap, fmt);
	vfprintf(logf, fmt, ap);
	va_end(ap);
	fputc('\n', logf);
	fflush(logf);
}
void Sys_LogClose(void)
{
	extern struct Library *LowLevelBase;
	if (LowLevelBase) { CloseLibrary(LowLevelBase); LowLevelBase = NULL; }
	{ extern void Sys_TimerClose(void); Sys_TimerClose(); }
	if (logf) { fclose(logf); logf = NULL; }
}
void Sys_MakeDir(const char *path)
{
	BPTR l = Lock((STRPTR)path, ACCESS_READ);
	if (!l) { l = CreateDir((STRPTR)path); if (l) Sys_Log("created %s", path); }
	if (l) UnLock(l);
}
void Sys_LogStart(int argc)
{
	Sys_Log("%s: %s start, attnflags %04x, chip free %lu, fast free %lu", ver + 6,
		argc ? "Shell" : "Workbench", (unsigned)SysBase->AttnFlags, (unsigned long)AvailMem(MEMF_CHIP), (unsigned long)AvailMem(MEMF_FAST));
}

extern struct Custom custom;
extern struct CIA ciaa;

static struct Screen *scr;
static struct Window *win;
/* Triple buffering: with two buffers the SafeMessage for the old buffer comes a
 * whole frame after the flip, so every frame waited two VBlanks (25 fps; found
 * with the PERF figures on 13 Oct 2026). With three, the buffer we draw into was
 * shown two flips ago and is normally safe already. */
#define NBUF 3
static struct ScreenBuffer *sb[NBUF];
static struct MsgPort *safeport, *dispport;
static int buf_safe[NBUF];        /* 1: free to draw into */
static int safe_for[NBUF];        /* sb[i]'s SafeMessage frees buffer safe_for[i] */
static int safe_out, disp_out;    /* replies still outstanding */
/* 15 Oct 2026 (A1200 PiStorm freeze with SKIP=0): a ScreenBuffer must not be
 * passed to ChangeScreenBuffer again until both messages of its previous change
 * are back, or graphics.library re-sends a message that is still queued and
 * the port list is corrupted. These flags say which are still out. */
static int safe_pend[3], disp_pend[3];
static struct BitMap *own_bm[3];   /* buffers 1-2: allocated by us with the screen's layout */
static int shown;                 /* buffer on screen (or about to be) */
static int flipped;               /* a ChangeScreenBuffer of this screen succeeded (shown is valid) */
static int CW, CH;                /* chunky size */
static int SW, SH;                /* screen size (<= chunky) */
static int XOFF, YOFF;            /* centring of the screen in the chunky buffer */
/* Per chunky line, bit b set = buffer b still has to be converted. Lines are
 * marked by the core as it copies a changed line (Render_LineChangedHook); this
 * replaced a per-buffer shadow copy and a full-screen compare every frame,
 * ~15 ms a frame on BigMig (13 Oct 2026). */
#define ALL_BUFFERS ((1 << NBUF) - 1)
static unsigned char *line_dirty;
static ULONG pal32[1 + 256 * 3 + 1];
static int pal_dirty, pal_dirty_all, pal_lo, pal_hi;
/* heartbeat counters (15 Oct 2026: SKIP=0 freezes on the A1200 PiStorm) */
static unsigned long hb_safe_waits, hb_refused, hb_reuse_waits;
static int hb_log_waits;
int Sys_PalAll;   /* PALALL argument (15 Oct 2026: GnG colour scramble test on the A1200) */
int Sys_AllLines, Sys_SafeFlip;   /* ALLLINES / SAFEFLIP (17 Oct 2026: torn frames on the A1200) */
int Sys_Osd, Sys_OsdSkip;         /* OSD (18 Oct 2026): which test mode a photo or video shows */

/* The label, drawn into the buffer about to be shown, top left, in palette
 * entry SYS_OSD_PEN on black: e.g. "SKIP3 SAFEFLIP". The c2p only rewrites
 * changed lines, so it is drawn again every shown frame. */
static void draw_osd(struct BitMap *bm)
{
	struct RastPort rp;
	char t[48];
	sprintf(t, "SKIP%d%s%s%s", Sys_OsdSkip, Sys_SafeFlip ? " SAFEFLIP" : "", Sys_AllLines ? " ALLLINES" : "", Sys_PalAll ? " PALALL" : "");
	InitRastPort(&rp);
	rp.BitMap = bm;
	SetFont(&rp, GfxBase->DefaultFont);
	SetAPen(&rp, SYS_OSD_PEN); SetBPen(&rp, 0); SetDrMd(&rp, JAM2);
	Move(&rp, 2, 2 + rp.TxBaseline);
	Text(&rp, (STRPTR)t, strlen(t));
}
static unsigned long frames;
static UWORD *blankptr;
#define KQ 128
static UWORD keyq[KQ], keyqual[KQ];
static int kq_head, kq_tail;
static int quit_requested;

/* c2p tables: bit p of a pixel goes to bit 8*p (planes 0-3) or 8*(p-4) (4-7) */
static ULONG c2p_lo[256], c2p_hi[256];

static void c2p_init(void)
{
	int v, p;
	for (v = 0; v < 256; v++) {
		ULONG lo = 0, hi = 0;
		for (p = 0; p < 4; p++) {
			if (v & (1 << p)) lo |= 1UL << (8 * p);
			if (v & (16 << p)) hi |= 1UL << (8 * p);
		}
		c2p_lo[v] = lo; c2p_hi[v] = hi;
	}
}

/* Convert one line of SW pixels into the 8 planes of a bitmap row.
 * Plain C, 32 pixels per step; to be replaced by an asm c2p once it runs. */
static void c2p_line(const unsigned char *src, struct BitMap *bm, int y)
{
	int p, g;
	UBYTE *plane[8];
	for (p = 0; p < 8; p++) plane[p] = bm->Planes[p] + (LONG)y * bm->BytesPerRow;
	for (g = 0; g < SW; g += 32) {
		ULONG out[8] = {0,0,0,0,0,0,0,0};
		int k;
		for (k = 0; k < 4; k++) {           /* 4 groups of 8 pixels -> 1 byte per plane */
			ULONG lo, hi;
			lo  = c2p_lo[src[0]] << 7; hi  = c2p_hi[src[0]] << 7;
			lo |= c2p_lo[src[1]] << 6; hi |= c2p_hi[src[1]] << 6;
			lo |= c2p_lo[src[2]] << 5; hi |= c2p_hi[src[2]] << 5;
			lo |= c2p_lo[src[3]] << 4; hi |= c2p_hi[src[3]] << 4;
			lo |= c2p_lo[src[4]] << 3; hi |= c2p_hi[src[4]] << 3;
			lo |= c2p_lo[src[5]] << 2; hi |= c2p_hi[src[5]] << 2;
			lo |= c2p_lo[src[6]] << 1; hi |= c2p_hi[src[6]] << 1;
			lo |= c2p_lo[src[7]];      hi |= c2p_hi[src[7]];
			src += 8;
			out[0] = (out[0] << 8) | ( lo        & 0xff);
			out[1] = (out[1] << 8) | ((lo >>  8) & 0xff);
			out[2] = (out[2] << 8) | ((lo >> 16) & 0xff);
			out[3] = (out[3] << 8) | ((lo >> 24) & 0xff);
			out[4] = (out[4] << 8) | ( hi        & 0xff);
			out[5] = (out[5] << 8) | ((hi >>  8) & 0xff);
			out[6] = (out[6] << 8) | ((hi >> 16) & 0xff);
			out[7] = (out[7] << 8) | ((hi >> 24) & 0xff);
		}
		for (p = 0; p < 8; p++) { *(ULONG *)plane[p] = out[p]; plane[p] += 4; }
	}
}

/* Sonic speed (20 Oct 2026): chip RAM writes are the c2p's cost on the PiStorm
 * (about 1 us a long). The core keeps palette indices low (render5.c
 * PALGC_TOP), so most lines use only pixel values < 32 or < 64: then planes
 * 5-7 (or 6-7) are all zero and need not be written, as long as that buffer's
 * row already has them zero. line_lvl[buffer][row] = planes that may be
 * non-zero there (5, 6 or 8); a conversion writes planes 0..n-1 and clears the
 * ones from n up to the old level. NO_C2P5 (define): always 8 planes. */
static unsigned char *line_lvl[NBUF];

static int line_planes(const unsigned char *src)
{
	const ULONG *l = (const ULONG *)src;
	ULONG o = 0;
	int n;
	for (n = SW >> 2; n > 0; n -= 4, l += 4) o |= l[0] | l[1] | l[2] | l[3];
	if (!(o & 0xE0E0E0E0UL)) return 5;
	if (!(o & 0xC0C0C0C0UL)) return 6;
	return 8;
}

/* planes 0..np-1 from the pixels, planes np..nz-1 cleared (np 5 or 6) */
static void c2p_line_n(const unsigned char *src, struct BitMap *bm, int y, int np, int nz)
{
	int p, g;
	UBYTE *plane[8];
	for (p = 0; p < 8; p++) plane[p] = bm->Planes[p] + (LONG)y * bm->BytesPerRow;
	for (g = 0; g < SW; g += 32) {
		ULONG o0 = 0, o1 = 0, o2 = 0, o3 = 0, o4 = 0, o5 = 0;
		int k;
		for (k = 0; k < 4; k++) {
			ULONG lo, hi;
			lo  = c2p_lo[src[0]] << 7; hi  = c2p_hi[src[0]] << 7;
			lo |= c2p_lo[src[1]] << 6; hi |= c2p_hi[src[1]] << 6;
			lo |= c2p_lo[src[2]] << 5; hi |= c2p_hi[src[2]] << 5;
			lo |= c2p_lo[src[3]] << 4; hi |= c2p_hi[src[3]] << 4;
			lo |= c2p_lo[src[4]] << 3; hi |= c2p_hi[src[4]] << 3;
			lo |= c2p_lo[src[5]] << 2; hi |= c2p_hi[src[5]] << 2;
			lo |= c2p_lo[src[6]] << 1; hi |= c2p_hi[src[6]] << 1;
			lo |= c2p_lo[src[7]];      hi |= c2p_hi[src[7]];
			src += 8;
			o0 = (o0 << 8) | ( lo        & 0xff);
			o1 = (o1 << 8) | ((lo >>  8) & 0xff);
			o2 = (o2 << 8) | ((lo >> 16) & 0xff);
			o3 = (o3 << 8) | ((lo >> 24) & 0xff);
			o4 = (o4 << 8) | ( hi        & 0xff);
			o5 = (o5 << 8) | ((hi >>  8) & 0xff);
		}
		*(ULONG *)plane[0] = o0; *(ULONG *)plane[1] = o1; *(ULONG *)plane[2] = o2;
		*(ULONG *)plane[3] = o3; *(ULONG *)plane[4] = o4;
		if (np > 5) *(ULONG *)plane[5] = o5;
		for (p = np; p < nz; p++) *(ULONG *)plane[p] = 0;
		for (p = 0; p < 8; p++) plane[p] += 4;
	}
}

/* Shadow (20 Oct 2026, the AmiMSX Space Manbow technique): a fast-RAM copy
 * of every buffer's planes. A converted line is compared with it a long at a
 * time and only longs that differ are written to chip RAM; sky, flat ground
 * and lines where only a sprite moved cost a few writes. line_lvl 0 = this
 * row's shadow is not known (new screen, OSD label): all 8 planes written.
 * NO_SHADOW (define): every long of the used planes written, as step 1. */
static ULONG *shadow[NBUF];          /* [row][plane][SW/32] */

#ifndef NO_EMUBR
#include "c2p_sh.h"
#endif
static void c2p_line_sh(const unsigned char *src, struct BitMap *bm, int y, int np, ULONG *sh, int known)
{
	int p, g, ng = SW >> 5;
	UBYTE *plane[8];
	unsigned long w = 0;
	for (p = 0; p < 8; p++) plane[p] = bm->Planes[p] + (LONG)y * bm->BytesPerRow;
#ifndef NO_EMUBR
	if (__builtin_expect(known != 0, 1))
	{
		Sys_Perf.c2p_chip_longs += c2p_sh_known(src, plane, ng, sh);
		return;
	}
#endif
	for (g = 0; g < ng; g++) {
		ULONG o[8];
		int k;
		o[0] = o[1] = o[2] = o[3] = o[4] = o[5] = o[6] = o[7] = 0;
		for (k = 0; k < 4; k++) {
			ULONG lo, hi;
			lo  = c2p_lo[src[0]] << 7; hi  = c2p_hi[src[0]] << 7;
			lo |= c2p_lo[src[1]] << 6; hi |= c2p_hi[src[1]] << 6;
			lo |= c2p_lo[src[2]] << 5; hi |= c2p_hi[src[2]] << 5;
			lo |= c2p_lo[src[3]] << 4; hi |= c2p_hi[src[3]] << 4;
			lo |= c2p_lo[src[4]] << 3; hi |= c2p_hi[src[4]] << 3;
			lo |= c2p_lo[src[5]] << 2; hi |= c2p_hi[src[5]] << 2;
			lo |= c2p_lo[src[6]] << 1; hi |= c2p_hi[src[6]] << 1;
			lo |= c2p_lo[src[7]];      hi |= c2p_hi[src[7]];
			src += 8;
			o[0] = (o[0] << 8) | ( lo        & 0xff);
			o[1] = (o[1] << 8) | ((lo >>  8) & 0xff);
			o[2] = (o[2] << 8) | ((lo >> 16) & 0xff);
			o[3] = (o[3] << 8) | ((lo >> 24) & 0xff);
			o[4] = (o[4] << 8) | ( hi        & 0xff);
			if (np > 5) {
				o[5] = (o[5] << 8) | ((hi >>  8) & 0xff);
				if (np > 6) {
					o[6] = (o[6] << 8) | ((hi >> 16) & 0xff);
					o[7] = (o[7] << 8) | ((hi >> 24) & 0xff);
				}
			}
		}
		for (p = 0; p < 8; p++) {
			ULONG *s = &sh[p * ng + g];
			if (!known || *s != o[p]) {
				*s = o[p];
				*(ULONG *)(plane[p] + (g << 2)) = o[p];
				w++;
			}
		}
	}
	Sys_Perf.c2p_chip_longs += w;
}

/* Vertical shift (vscroll, 20 Oct 2026). When the picture moves up or down
 * (Sonic GX's springs), nearly every line differs from what the buffer holds,
 * and the shadow compare writes almost the whole screen. Instead, each buffer
 * is a window into a taller chip area (VS_MARGIN spare rows above and below):
 * the window's bitmap is a copy of the tall bitmap's header whose plane
 * pointers start win_row[b] rows down. Moving the window by d rows makes the
 * rows already in chip RAM appear d rows higher or lower, at no cost (the
 * display just fetches from another address); the shadow, the plane levels and
 * the row hashes are all kept per tall row, so they move with it, and the
 * usual compare then writes only what differs (the new edge, sprites, the HUD).
 * Exact by construction: the shadow always equals chip RAM, and every row of a
 * moved window is compared, so a wrong guess only costs writes.
 * Finding d: for every chunky line a hash (recomputed only for lines the core
 * changed); each tall row keeps the hash of the line it holds. When at least
 * a quarter of the rows are dirty, the rows to be written vote for an offset
 * by looking their new line up among the tall rows near the window
 * (vs_choose), and the window moves if that offset puts at least SH/16 more
 * whole rows in place. A window near the end of its area returns to the
 * middle on a frame that rewrites most rows anyway.
 * NO_VSHIFT (define) or NOVSHIFT (argument): plain buffers, as 0.7. */
#if !defined(NO_SHADOW) && !defined(NO_VSHIFT)
#define VSHIFT 1
#define VS_MARGIN 128
#define VS_DMAX 96
#endif
int Sys_VShiftOn, Sys_NoVShift, Sys_VShiftOn_Last;
int Sys_TallScreen, Sys_WideScreen;
static int TR;                       /* rows in each buffer's chip area (SH without vshift) */
static int win_row[NBUF];            /* first row of the window shown */
#ifdef VSHIFT
static struct BitMap *tall_bm[NBUF];
static PLANEPTR sb_planes0[NBUF][8];   /* the plane pointers Intuition gave each buffer, put back before freeing */
static struct ScreenBuffer *sb_scr;  /* the screen's own bitmap, shown again before closing */
static ULONG *row_hash[NBUF];        /* [TR] hash of the chunky line each tall row holds */
static ULONG *chash;                 /* [CH] the chunky lines' hashes */
static unsigned char *chash_stale;   /* [CH] changed since its hash was taken */

/* every other long of the line (a missed difference only costs writes) */
static ULONG hash_line(const unsigned char *src)
{
	const ULONG *l = (const ULONG *)src;
	ULONG x = 0x9e3779b9UL;
	int n;
	for (n = SW >> 3; n > 0; n--, l += 2) x = ((x << 5) | (x >> 27)) ^ l[0];
	return x;
}

/* Finding d by votes (A1200, 20 Oct 2026: a search over every offset cost
 * 0.27 ms a frame, more than it saved): the hashes of the known tall rows
 * around the window go into a small open-addressing table (a hash seen twice,
 * e.g. plain sky, is marked ambiguous); each row that has to be written looks
 * its new line up there, and a hit at tall row t votes for d = t - r - y.
 * The winner is checked by counting whole rows that would match at d and at
 * the current position. */
#define VS_HT 1024
static ULONG vs_hkey[VS_HT];
static short vs_hrow[VS_HT];          /* -1 empty, -2 ambiguous, else tall row */
static int vs_matches(int b, int t0)
{
	int y, n = 0;
	const unsigned char *lv = line_lvl[b] + t0;
	const ULONG *a = row_hash[b] + t0, *c = chash + YOFF;
	for (y = 0; y < SH; y++) n += lv[y] && a[y] == c[y];
	return n;
}
static int vs_choose(int b, int ndirty)
{
	int r = win_row[b], lo, hi, t, y, d, best = 0, bv = 0, m0, m;
	unsigned short votes[2 * VS_DMAX + 1];
	if (ndirty < SH / 4) return r;
	Sys_Perf.vs_searches++;
	lo = r - VS_DMAX; if (lo < 0) lo = 0;
	hi = r + SH + VS_DMAX; if (hi > TR) hi = TR;
	for (t = 0; t < VS_HT; t++) vs_hrow[t] = -1;
	for (t = lo; t < hi; t++) {
		ULONG h = row_hash[b][t];
		int i = (int)((h ^ (h >> 13)) & (VS_HT - 1));
		if (!line_lvl[b][t]) continue;
		while (vs_hrow[i] != -1 && vs_hkey[i] != h) i = (i + 1) & (VS_HT - 1);
		if (vs_hrow[i] == -1) { vs_hkey[i] = h; vs_hrow[i] = t; }
		else vs_hrow[i] = -2;
	}
	memset(votes, 0, sizeof votes);
	for (y = 0; y < SH; y++) {
		ULONG h = chash[y + YOFF];
		int i = (int)((h ^ (h >> 13)) & (VS_HT - 1));
		if (line_lvl[b][r + y] && row_hash[b][r + y] == h) continue;   /* already right where it is */
		while (vs_hrow[i] != -1 && vs_hkey[i] != h) i = (i + 1) & (VS_HT - 1);
		if (vs_hrow[i] < 0) continue;
		d = vs_hrow[i] - r - y;
		if (d && d >= -VS_DMAX && d <= VS_DMAX) votes[d + VS_DMAX]++;
	}
	for (d = -VS_DMAX; d <= VS_DMAX; d++)
		if (votes[d + VS_DMAX] > bv) { bv = votes[d + VS_DMAX]; best = d; }
	m0 = vs_matches(b, r);
	if (bv >= 4 && r + best >= 0 && r + best + SH <= TR) {
		m = vs_matches(b, r + best);
		if (m >= m0 + SH / 16) { Sys_Perf.vs_shifts++; return r + best; }
	}
	if (m0 < SH / 4 && (r < VS_MARGIN / 2 || r > VS_MARGIN + VS_MARGIN / 2)) { Sys_Perf.vs_recentres++; return VS_MARGIN; }
	return r;
}
#endif   /* VSHIFT; AUDIT works with every buffer kind (NO_VSHIFT / NO_SHADOW builds link again) */

/* AUDIT (vscroll, 20 Oct 2026; reassembler N5): after each conversion the
 * whole buffer is read back from chip RAM and compared with a plain c2p of
 * the chunky frame (rows under the OSD label excepted), and the hardware
 * copper list's BPL1PT is compared with the plane pointer of the buffer the
 * display should show. Slow (reads chip RAM); counts go into the perf report. */
int Sys_Audit;
static void audit_frame(const unsigned char *chunky, int pitch, struct BitMap *bm, int b)
{
	int y, g, p, k, bad = 0, y0 = Sys_Osd ? 12 : 0;
	for (y = y0; y < SH; y++) {
		const unsigned char *src = chunky + (y + YOFF) * pitch + XOFF;
		int rowbad = 0;
		for (g = 0; g < (SW >> 5); g++) {
			ULONG o[8];
			for (p = 0; p < 8; p++) o[p] = 0;
			for (k = 0; k < 32; k++) { int v = src[(g << 5) + k]; for (p = 0; p < 8; p++) o[p] = (o[p] << 1) | ((v >> p) & 1); }
			for (p = 0; p < 8; p++)
				if (*(ULONG *)(bm->Planes[p] + (LONG)y * bm->BytesPerRow + (g << 2)) != o[p]) { rowbad++; break; }
		}
		if (rowbad && Sys_Perf.audit_bad_groups < 40) Sys_Log("audit: frame %lu buffer %d row %d (tall %d): %d groups differ", frames, b, y, win_row[b] + y, rowbad);
		bad += rowbad;
	}
	Sys_Perf.audit_frames++;
	if (bad) { Sys_Perf.audit_bad_frames++; Sys_Perf.audit_bad_groups += bad; }
}
static void audit_copper(void)
{
	struct View *v = GfxBase->ActiView;
	struct cprlist *c;
	UWORD *w;
	ULONG hi = 0, want, want2;
	int n;
	/* the display shows the last buffer passed to ChangeScreenBuffer once its
	 * DispMessage is back, else possibly still the one before it */
	if (frames < 3 || !v || !(c = v->LOFCprList) || !c->start) return;
	want = (ULONG)sb[shown]->sb_BitMap->Planes[0];
	want2 = disp_pend[shown] ? (ULONG)sb[(shown + NBUF - 1) % NBUF]->sb_BitMap->Planes[0] : want;
	for (w = c->start, n = 0; n < c->MaxCount; n++, w += 2) {
		if (w[0] == 0x00e0) hi = w[1];
		else if (w[0] == 0x00e2) {
			ULONG a = (hi << 16) | w[1];
			Sys_Perf.audit_cop_checks++;
			if (a != want && a != want2) {
				if (Sys_Perf.audit_cop_bad < 20) Sys_Log("audit: copper BPL1PT %08lx, buffer %d planes %08lx (window row %d)", (unsigned long)a, shown, (unsigned long)want, win_row[shown]);
				Sys_Perf.audit_cop_bad++;
			}
			return;
		}
	}
}

#ifdef VSHIFT
static void vs_place(int b, int r)
{
	struct BitMap *t = tall_bm[b];
	int p;
	win_row[b] = r;
	for (p = 0; p < 8; p++) sb[b]->sb_BitMap->Planes[p] = t->Planes[p] + (LONG)r * t->BytesPerRow;
}
#endif

/* ---- video mode and display rate (19 Oct 2026, John's A1200 in NTSC) ----
 * Sys_VideoMode: 0 Auto (the default monitor, as Workbench), 1 PAL, 2 NTSC.
 * A forced PAL or NTSC mode that the system can't show (its monitor driver
 * not mounted) falls back to Auto. */
int Sys_VideoMode;
unsigned long Sys_DisplayFrameUs = 20000;   /* measured VBlank interval of the last screen */
static ULONG measured_mode = 0xffffffff;

ULONG Sys_ModeID(ULONG key)
{
	ULONG id = key;
	if (Sys_VideoMode == 1) id = PAL_MONITOR_ID | key;
	else if (Sys_VideoMode == 2) id = NTSC_MONITOR_ID | key;
	if (id != key && ModeNotAvailable(id)) {
		Sys_Log("display: mode %08lx not available, using the default monitor", (unsigned long)id);
		id = key;   /* DEFAULT_MONITOR_ID is 0 */
	}
	return id;
}

/* The VBlank interval of the screen just opened, timed with the E-clock over
 * six frames (once per mode: Continue doesn't pay for it again). Whatever
 * the mode ID says, this is the rate the display really runs at. */
static void measure_vblank(ULONG modeid)
{
	unsigned long t0, t1;
	int i;
	if (modeid == measured_mode) return;
	WaitTOF();
	t0 = Sys_Micros();
	for (i = 0; i < 6; i++) WaitTOF();
	t1 = Sys_Micros();
	if (t1 > t0) Sys_DisplayFrameUs = (t1 - t0) / 6;
	measured_mode = modeid;
#ifndef NO_PALBEAM
	/* A PAL screen whose beam runs at 60 Hz (21 Oct 2026, John's A1200: 16.86 ms
	 * VBlank for 0.8 and 0.9 alike, PAL DisplayFlags): another program wrote
	 * BEAMCON0 = NTSC and left it (AmiMSX does for 60 Hz MSX games), and graphics
	 * never rewrites it for a PAL view on a PAL machine. The 256-line screen then
	 * sits in a 263-line frame: its bottom ~38 lines are lost and the picture
	 * looks low. Put the PAL beam back (what the machine booted with). Not for an
	 * NTSC mode (VIDEO=NTSC) or an NTSC machine. NO_PALBEAM: leave it. */
	if (!Sys_RtgActive && !Sys_Display50Hz() && (GfxBase->DisplayFlags & PAL)
	 && ((modeid & MONITOR_ID_MASK) == PAL_MONITOR_ID || (modeid & MONITOR_ID_MASK) == DEFAULT_MONITOR_ID)) {
		unsigned long was = Sys_DisplayFrameUs;
		custom.beamcon0 = DISPLAYPAL;
		WaitTOF(); WaitTOF();
		t0 = Sys_Micros();
		for (i = 0; i < 6; i++) WaitTOF();
		t1 = Sys_Micros();
		if (t1 > t0) Sys_DisplayFrameUs = (t1 - t0) / 6;
		Sys_Log("display: PAL screen on a 60 Hz beam (VBlank %lu us; BEAMCON0 left NTSC by another program): PAL beam restored, VBlank now %lu us",
			was, Sys_DisplayFrameUs);
	}
#endif
	Sys_Log("display: VBlank every %lu us (%s), mode %08lx, viewport mode %08lx, gfx DisplayFlags %04x, E-clock %lu Hz",
		Sys_DisplayFrameUs, Sys_Display50Hz() ? "50 Hz" : "not 50 Hz", (unsigned long)modeid,
		(unsigned long)GetVPModeID(&scr->ViewPort), (unsigned)GfxBase->DisplayFlags, (unsigned long)SysBase->ex_EClockFrequency);
}

int Sys_Display50Hz(void) { return Sys_DisplayFrameUs > 18500 && Sys_DisplayFrameUs < 22000; }

/* Paula's clock: 5 x the E-clock (PAL 3546895 Hz, NTSC 3579545 Hz). It comes
 * from the machine's crystal, not the display mode: a PAL A1200 showing NTSC
 * still has the PAL clock. */
unsigned long Sys_ChipClock(void)
{
	unsigned long e = SysBase->ex_EClockFrequency;
	if (e < 600000 || e > 800000) e = 709379;
	return e * 5;
}

/* ---- RTG screen (pilot, 26 Oct 2026; DESIGN.md 9.2 of rtg-framework) ----
 * The single-buffer way (John's choice after the A1200 bench: ChangeScreenBuffer waits for
 * the 60 Hz HDMI flip there, ~8-9 ms): rtgfw opens one 8-bit CLUT RTG screen and a backdrop
 * window, and every shown frame writes the core's changed chunky lines straight into that
 * screen's bitmap under a lock (path A + C), with the changed palette range loaded once a
 * frame. The chunky frame is shown whole (384x272, or 768x272 hires at 1:2), centred in the
 * smallest fitting mode, the surround in pen 0 = the CPC border colour (the colour-0 rule).
 * No c2p, shadow, vertical shift or AGA buffers. Tearing is possible (no flip). Pacing and
 * Paula are untouched: the native VBlank keeps running under an RTG screen and is measured
 * as for AGA. RTGBLIT: an off-screen bitmap and a blit (for comparison); RTGBUF=3: three
 * screen buffers. If no RTG board or mode is found, the AGA screen opens as before. */
#include "rtgfw/src/rtgfw.h"
#include "screenshot.h"
int Sys_RtgWanted, Sys_RtgActive, Sys_RtgBlit, Sys_RtgBuffers = 1, Sys_RtgGrabOn;
/* Scanlines on the RTG screen (1.2, 26 Oct 2026; rtgfw RtgOpen.scan): SYS_SCAN_* = the Scanlines
 * setting. Off is the 1x picture as before. The others draw the picture at 2x (each CPC pixel
 * 2x2) into a bigger RTG mode, the lower row of each pair being the scanline: hard = the line
 * dimmed (8-bit CLUT: dimmed colours in CLUT 128-255), blended = the line and the line below
 * averaged and dimmed (16/32-bit screen). The mode: 960x540 if there is one (exactly 2x to a
 * 1080p HDMI output: the PiStorm's hdmi_mode=82; 270 of the 272 lines fit, the top and bottom
 * border lines are cut), else the smallest mode of 768x544 or more (768x576, 800x600, ...).
 * RTGDEPTH=8|16|32 and SCANMODE=<w>x<h> (tests) override the depth and the mode size. */
int Sys_RtgScan, Sys_RtgDepth, Sys_RtgScanW, Sys_RtgScanH;
static const unsigned char scan_kind[SYS_SCANS] = { RTG_SCAN_OFF, RTG_SCAN_HARD, RTG_SCAN_HARD, RTG_SCAN_HARD, RTG_SCAN_BLEND, RTG_SCAN_BLEND, RTG_SCAN_BLEND };
static const unsigned char scan_pct[SYS_SCANS] = { 0, 25, 50, 75, 25, 50, 75 };
#define BLEND_DEPTH 32                /* blended scanlines: 32-bit (exact colours); RTGDEPTH=16 for RGB565 */
static int rtg_scan_open;             /* the scanline setting of the RTG screen open now (for the log at close) */
int Sys_RtgDead;                    /* RTG failed once (screen or menu): AGA is used until the user ticks RTG again */
char Sys_RtgStatus[80];
static int rtg_avail = -1;          /* -1: not asked yet */
static RtgOpen rtg_o;
static unsigned long rtg_errs;
/* Hires (768x544 chunky, render5.c ScanLines): since 1.2 the core's line-changed hook names
 * the surface row a line was copied to (Render_ScreenYOffset + 2i), so hires uses the line
 * marks like lores; the pilot's "every line every frame" workaround is gone. */

static void rtg_log(const char *line) { Sys_Log("%s", line); }

int Sys_RtgAvailable(void)
{
	if (rtg_avail < 0) {
		RtgCaps c;
		int e;
		memset(&c, 0, sizeof c);
		c.size = sizeof c;
		e = rtg_query(&c);
		rtg_avail = e == RTG_OK && (c.depths & 1);
		Sys_Log("rtg: query: %s, system %d, depths %d, library v%d, board '%s': 8-bit RTG screen %s",
			e == RTG_OK ? "ok" : rtg_error_text(e), (int)c.system, (int)c.depths, (int)c.lib_version, c.board, rtg_avail ? "available" : "not available");
	}
	return rtg_avail;
}

unsigned long Sys_RtgMenuMode(int *w, int *h)
{
	/* ALL the modes: the PiStorm's VideoCore lists 588 (the pilot read only the first 128, the
	 * small ones, so on the real A1200 no 640-wide 8-bit mode was ever found and the menu opened
	 * on the AGA screen: found 26 Oct 2026 at the first double-click of the Arnold RTG icon) */
	enum { MAXMODES = 2048 };
	RtgModeInfo *m;
	int n, i, best = -1;
	if (!Sys_RtgAvailable()) return 0;
	m = AllocVec(MAXMODES * sizeof(RtgModeInfo), MEMF_ANY);
	if (!m) { Sys_Log("rtg: no memory for the mode list"); return 0; }
	n = rtg_x_modes(m, MAXMODES);
	Sys_Log("rtg: %d RTG modes listed for the menu screen", n);
	for (i = 0; i < n; i++) {
		if (m[i].fmt != RTG_FMT_CLUT8 || m[i].w < 640 || m[i].h < 256) continue;
		if (best < 0 || (long)m[i].w * m[i].h < (long)m[best].w * m[best].h) best = i;
	}
	if (best < 0) { FreeVec(m); Sys_Log("rtg: no 8-bit RTG mode of 640x256 or more for the menu"); return 0; }
	*w = m[best].w; *h = m[best].h;
	{ unsigned long id = m[best].id; FreeVec(m); return id; }
}

/* the scanline mode: SCANMODE's size, else 960x540, in the wanted depth (0 if none: rtgfw picks
 * the smallest mode of at least 2x the picture) */
static unsigned long rtg_scan_mode(int depth, int *w, int *h)
{
	enum { MAXMODES = 2048 };
	RtgModeInfo *m;
	int n, i, ww = Sys_RtgScanW ? Sys_RtgScanW : 960, wh = Sys_RtgScanH ? Sys_RtgScanH : 540;
	unsigned long id = 0;
	m = AllocVec(MAXMODES * sizeof(RtgModeInfo), MEMF_ANY);
	if (!m) return 0;
	n = rtg_x_modes(m, MAXMODES);
	for (i = 0; i < n && !id; i++) {
		int g = m[i].fmt == RTG_FMT_CLUT8 ? 8 : (m[i].fmt == RTG_FMT_RGB565 || m[i].fmt == RTG_FMT_RGB565PC) ? 16 : m[i].fmt >= RTG_FMT_ARGB32 && m[i].fmt <= RTG_FMT_RGBA32 ? 32 : 0;
		if (g == depth && m[i].w == ww && m[i].h == wh) { id = m[i].id; *w = m[i].w; *h = m[i].h; }
	}
	FreeVec(m);
	if (!id) Sys_Log("rtg: scanlines: no %dx%d %d-bit mode, the smallest of 768x544 or more", ww, wh, depth);
	return id;
}

static int rtg_open_display(int cw, int ch, int hires)
{
	int e, sc = Sys_RtgScan > 0 && Sys_RtgScan < SYS_SCANS ? Sys_RtgScan : 0;
	memset(&rtg_o, 0, sizeof rtg_o);
	rtg_o.size = sizeof rtg_o; rtg_o.api = RTGFW_API;
	rtg_o.src_w = cw; rtg_o.src_h = ch; rtg_o.vis_w = cw; rtg_o.vis_h = ch;   /* all of it: no WIDE/TALL crops */
	rtg_o.aspect_x = 1; rtg_o.aspect_y = (hires && ch <= 300) ? 2 : 1;   /* the hires chunky frame is 768x544: its lines are doubled already */
	if (sc && ch > 300) { Sys_Log("rtg: scanlines are for the lores picture: off in hires"); sc = 0; }
	rtg_o.depth = scan_kind[sc] == RTG_SCAN_BLEND ? BLEND_DEPTH : 8;
	if (Sys_RtgDepth == 8 || Sys_RtgDepth == 16 || Sys_RtgDepth == 32) rtg_o.depth = Sys_RtgDepth;
	if (scan_kind[sc] == RTG_SCAN_BLEND && rtg_o.depth == 8) rtg_o.depth = 16;
	rtg_o.scan = scan_kind[sc]; rtg_o.scan_pct = scan_pct[sc];
	if (sc && Sys_RtgAvailable()) {
		int mw = 0, mh = 0;
		unsigned long id = rtg_scan_mode(rtg_o.depth, &mw, &mh);
		if (id) {
			/* the picture at 2x in that mode: what does not fit is cut evenly (960x540: 270 lines, 1 off top and bottom) */
			rtg_o.mode_id = id;
			if (rtg_o.vis_h * 2 > mh) { rtg_o.vis_h = mh / 2; rtg_o.vis_y = (ch - rtg_o.vis_h) / 2; }
			if (rtg_o.vis_w * 2 > mw) { rtg_o.vis_w = mw / 2; rtg_o.vis_x = (cw - rtg_o.vis_w) / 2; }
		}
	}
	rtg_o.buffers = Sys_RtgBlit ? 1 : (Sys_RtgBuffers >= 1 && Sys_RtgBuffers <= 3 ? Sys_RtgBuffers : 1);
	rtg_o.border_pen = 0;
	rtg_o.flags = RTG_F_LOG | (Sys_RtgBlit ? RTG_F_BLIT : 0);
	rtg_o.content_hz100 = 5000;
	rtg_o.log = rtg_log;
	if (!Sys_RtgAvailable()) e = RTG_E_NOMODE;
	else e = rtg_open(&rtg_o);
	if (e != RTG_OK) {
		Sys_Log("rtg: open failed (%s): using the AGA screen", rtg_error_text(e));
		snprintf(Sys_RtgStatus, sizeof Sys_RtgStatus, "RTG screen: %s, AGA used.",
			e == RTG_E_NOLIB || e == RTG_E_NOMODE ? "no RTG board or 8-bit mode" : rtg_error_text(e));
		if (e == RTG_E_NOLIB || e == RTG_E_NOMODE) rtg_avail = 0;
		Sys_RtgDead = 1;                  /* fall back once: no retry on every Help */
		return 0;
	}
	line_dirty = AllocMem(CH, MEMF_ANY);
	if (!line_dirty) { rtg_close(); strcpy(Sys_RtgStatus, "RTG screen: out of memory, AGA used."); Sys_RtgDead = 1; return 0; }
	memset(line_dirty, 1, CH);            /* the first present writes every line */
	scr = rtg_o.out_screen;
	win = rtg_o.out_window;
	/* as the AGA window: take wheel events ourselves (Intuition would make cursor keys of them) */
	ModifyIDCMP(win, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS | IDCMP_EXTENDEDMOUSE);
	Sys_RtgActive = 1;
	Sys_VShiftOn = 0; Sys_VShiftOn_Last = 0;
	SW = cw; SH = ch; XOFF = YOFF = 0;
	flipped = 0; shown = 0; safe_out = disp_out = 0;
	pal32[0] = 256UL << 16;
	pal32[1 + 256 * 3] = 0;
	pal32[1 + SYS_OSD_PEN * 3] = 0xFFFFFFFF; pal32[2 + SYS_OSD_PEN * 3] = 0xDDDDDDDD; pal32[3 + SYS_OSD_PEN * 3] = 0;
	pal_dirty = 1; pal_dirty_all = 1;
	rtg_errs = 0;
	Sys_Log("rtg: screen %08lx %dx%d, picture %dx%d at %d,%d (x%d,%d), format %d, %s, chip free %lu", (unsigned long)rtg_o.out_mode_id,
		(int)rtg_o.out_w, (int)rtg_o.out_h, cw * rtg_o.out_scale_x, ch * rtg_o.out_scale_y, (int)rtg_o.out_x, (int)rtg_o.out_y,
		(int)rtg_o.out_scale_x, (int)rtg_o.out_scale_y, (int)rtg_o.out_fmt,
		rtg_o.out_buffers == 0 ? "off-screen bitmap + blit" : rtg_o.out_buffers == 1 ? "single buffer, direct writes" : "screen buffers",
		(unsigned long)AvailMem(MEMF_CHIP));
	rtg_scan_open = sc;
	if (sc) Sys_Log("rtg: scanlines %s (%s %d%%), lines %d-%d of the frame shown", Sys_ScanWords[sc],
		scan_kind[sc] == RTG_SCAN_HARD ? "hard" : "blended", (int)scan_pct[sc], (int)rtg_o.vis_y, (int)(rtg_o.vis_y + rtg_o.vis_h - 1));
	measure_vblank(rtg_o.out_mode_id);    /* the native VBlank: pacing and Paula as on AGA */
	Sys_Log("display: open (RTG)");
	return 1;
}

static void rtg_show(const unsigned char *chunky, int pitch)
{
	static ULONG rgb[256];
	unsigned long t0 = Sys_Micros(), t1;
	RtgStats s0, s1;
	int e;
	rtg_get_stats(&s0);
	if (Sys_PalAll) pal_dirty = pal_dirty_all = 1;
	if (pal_dirty) {
		int i, lo, hi;
		if (pal_dirty_all) { pal_lo = 0; pal_hi = 255; pal_dirty_all = 0; }
		lo = pal_lo; hi = pal_hi;
		for (i = lo; i <= hi; i++)
			rgb[i] = ((pal32[1 + i * 3] >> 24) << 16) | ((pal32[2 + i * 3] >> 24) << 8) | (pal32[3 + i * 3] >> 24);
		rtg_set_palette(0, lo, hi - lo + 1, rgb + lo, RTG_RGB24);   /* loaded by rtg_present (LoadRGB32 of that range) */
		pal_dirty = 0;
		Sys_Perf.palette_loads++;
	}
	e = rtg_present(chunky, pitch, Sys_AllLines ? NULL : line_dirty);
	memset(line_dirty, 0, CH);            /* a skipped frame's lines stay owed inside rtgfw */
	if (e != RTG_OK && rtg_errs++ < 10) Sys_Log("rtg: present: %s (frame %lu)", rtg_error_text(e), frames);
	rtg_get_stats(&s1);
	Sys_Perf.c2p_lines += s1.lines_written - s0.lines_written;
	Sys_Perf.c2p_chip_longs += (s1.bytes_written - s0.bytes_written) >> 2;   /* RTG: bytes written to the board / 4 */
	Sys_Perf.c2p_frames++;
	Sys_Perf.line_bytes = SW;             /* one byte a pixel */
	frames++;
	t1 = Sys_Micros(); Sys_Perf.c2p_us += t1 - t0;
}

int Sys_RtgGrab(const char *name, const unsigned char *chunky, int pitch)
{
	static unsigned char pal[256][3];
	static ULONG clut[256];
	unsigned char *pens;
	int i, e, w, h;
	if (!Sys_RtgActive) return 0;
	rtg_show(chunky, pitch);              /* TURBO shows every 25th frame only: make sure this one is on the screen */
	w = rtg_o.out_w; h = rtg_o.out_h;
	if (rtg_o.out_fmt != RTG_FMT_CLUT8) {
		/* 16/32-bit screen (blended scanlines): the screen as RGB, saved as a PPM beside the name */
		uint32_t *rgb = AllocVec((long)w * h * 4, MEMF_ANY);
		char ppm[300];
		FILE *f;
		long k;
		if (!rgb) return 0;
		e = rtg_x_read_rgb(rgb);
		strncpy(ppm, name, sizeof ppm - 5); ppm[sizeof ppm - 5] = 0;
		{ char *dot = strrchr(ppm, '.'); if (dot) *dot = 0; }
		strcat(ppm, ".ppm");
		if (e == RTG_OK && (f = fopen(ppm, "wb"))) {
			unsigned char *row = (unsigned char *)rgb;   /* rewritten in place: 4 bytes in, 3 out */
			fprintf(f, "P6\n%d %d\n255\n", w, h);
			for (k = 0; k < (long)w * h; k++) { uint32_t v = rgb[k]; row[3 * k] = v >> 16; row[3 * k + 1] = v >> 8; row[3 * k + 2] = v; }
			fwrite(row, 3, (long)w * h, f);
			fclose(f);
		} else Sys_Log("rtg: grab %s: %s", ppm, e == RTG_OK ? "can't write" : rtg_error_text(e));
		FreeVec(rgb);
		return e == RTG_OK;
	}
	pens = malloc((long)w * h);
	if (!pens) return 0;
	e = rtg_x_read_frame(pens, (uint32_t *)clut);
	if (e == RTG_OK) {
		for (i = 0; i < 256; i++) { pal[i][0] = clut[i] >> 16; pal[i][1] = clut[i] >> 8; pal[i][2] = clut[i]; }
		Screenshot_SavePNG(name, pens, w, h, w, pal);
	} else Sys_Log("rtg: grab %s: %s", name, rtg_error_text(e));
	free(pens);
	return e == RTG_OK;
}

static void rtg_close_display(void)
{
	uint32_t hl = 0;
	if (rtg_scan_open && rtg_x_scan_info(&hl) == RTG_SCAN_HARD && hl && rtg_o.out_fmt == RTG_FMT_CLUT8)
		Sys_Log("rtg: scanlines: %lu lines had pens 128+ (their scanline is not dimmed)", (unsigned long)hl);
	rtg_close();                          /* waits for any buffer messages, closes the window and screen */
	scr = NULL; win = NULL;
	if (line_dirty) { FreeMem(line_dirty, CH); line_dirty = NULL; }
	Sys_RtgActive = 0;
	Sys_Log("display: closed (RTG)");
}

int Sys_OpenDisplay(int cw, int ch, int hires)
{
	struct Rectangle rect;
	ULONG modeid = Sys_ModeID(hires ? HIRES_KEY : LORES_KEY);
	int i, rw, rh, ntsc_max, wide_max;

	CW = cw; CH = ch;
	flipped = 0;
	c2p_init();
	quit_requested = 0; kq_head = kq_tail = 0;
	if (Sys_RtgWanted && !Sys_RtgDead && rtg_open_display(cw, ch, hires)) return 1;   /* else the AGA screen, as before */
	/* Standard overscan, not MAX: MAX's rectangle starts further left than a
	 * monitor shows (first WinUAE run: the left border was off the screen). */
	if (!QueryOverscan(modeid, &rect, OSCAN_STANDARD)) {
		rect.MinX = 0; rect.MinY = 0; rect.MaxX = (hires ? 703 : 351); rect.MaxY = 271;
	}
	/* TALL (Pinball Dreams, 20 Oct 2026): PAL standard overscan is 256 lines, so
	 * rows 8-263 of the 272 are shown; a game drawn from the top of the frame
	 * (Pinball Dreams' score row starts at row 2) loses its first lines. Full
	 * height takes the mode's full vertical overscan, up to all 272 rows. */
	/* NTSC (19 Oct 2026, John): standard overscan is only about 200 lines, which
	 * crops games drawn to the screen edges. Take the mode's full vertical
	 * overscan (OSCAN_MAX, about 240 lines; at most 240 here, centred in it)
	 * and keep the standard horizontal extent (MAX starts left of what a
	 * monitor shows). PAL keeps standard overscan, as before. */
	ntsc_max = 0;
	{
		struct DimensionInfo dims;
		struct Rectangle mx;
		if (GetDisplayInfoData(NULL, (UBYTE *)&dims, sizeof dims, DTAG_DIMS, modeid) > 0
		 && (dims.Nominal.MaxY - dims.Nominal.MinY + 1 < 256 || Sys_TallScreen)
		 && QueryOverscan(modeid, &mx, OSCAN_MAX) && mx.MaxY - mx.MinY > rect.MaxY - rect.MinY) {
			int h = mx.MaxY - mx.MinY + 1, cut = 0, hmax = Sys_TallScreen ? ch : 240;
			if (h > hmax) { cut = (h - hmax) / 2; h = hmax; }
			rect.MinY = mx.MinY + cut;
			rect.MaxY = rect.MinY + h - 1;
			ntsc_max = 1;
		}
	}
	/* WIDE (24 Oct 2026): PAL standard overscan is 320..352 lores columns, so
	 * 32 columns each side of the 384-wide CPC picture are lost (Sonic GX's ring
	 * icon, Pinball Dreams' logo wingtip). Wide takes the mode's full horizontal
	 * overscan (OSCAN_MAX: 362 lores pixels on PAL, hpos 93-455, the most the
	 * beam shows) as the display clip, as TALL does vertically. The screen is the
	 * whole 384 columns, wider than the clip, so Intuition starts the fetch (11
	 * columns in, by the screen's negative left edge) and the picture is centred
	 * in the window: columns 11-372 show. The hardware fetch (DDFSTRT, BPLCON1
	 * scroll, 64-pixel FMODE 3 blocks) is left to graphics.library. Lores only;
	 * hires screens keep their standard width. */
	wide_max = 0;
	if (Sys_WideScreen && !hires) {
		struct Rectangle mx;
		if (QueryOverscan(modeid, &mx, OSCAN_MAX) && mx.MaxX - mx.MinX > rect.MaxX - rect.MinX) {
			rect.MinX = mx.MinX;
			rect.MaxX = mx.MaxX;
			wide_max = 1;
		} else Sys_Log("display: WIDE: no wider overscan in mode %08lx, standard width", (unsigned long)modeid);
	}
	rw = rect.MaxX - rect.MinX + 1;
	rh = rect.MaxY - rect.MinY + 1;
	SW = (wide_max ? cw : rw < cw ? rw : cw) & ~31;   /* wide: the whole picture, centred in (and cropped by) the overscan window */
	SH = rh < ch ? rh : ch;
	Sys_Log("display: mode %08lx overscan%s %d,%d-%d,%d screen %dx%d chunky %dx%d chip free %lu", (unsigned long)modeid, ntsc_max ? (wide_max ? " (full height, wide)" : " (full height)") : (wide_max ? " (wide)" : ""),
		(int)rect.MinX, (int)rect.MinY, (int)rect.MaxX, (int)rect.MaxY, SW, SH, cw, ch, (unsigned long)AvailMem(MEMF_CHIP));
	XOFF = (cw - SW) / 2;
	YOFF = (ch - SH) / 2;
#ifdef YOFF7   /* opt-in: moves every PAL picture (Sonic too) up one line */
	/* PAL 256-line screen (21 Oct 2026): the usual full-height Plus picture
	 * (Sonic GX, Bomb Jack GX, Goldorak, Frogger, Picrocs ...) is drawn on
	 * chunky rows 7-262, so centring (rows 8-263) lost its top line (Bomb Jack's
	 * SIDE-ONE text tops) and showed an empty row 263. Rows 7-262 instead. */
	if (!ntsc_max && ch - SH == 16) YOFF = 7;
#endif

	scr = OpenScreenTags(NULL,
		SA_Width, SW, SA_Height, SH, SA_Depth, 8,
		SA_DisplayID, modeid,
		(ntsc_max || wide_max) ? SA_DClip : SA_Overscan, (ntsc_max || wide_max) ? (ULONG)&rect : OSCAN_STANDARD,
		SA_Left, ((ntsc_max || wide_max) ? rect.MinX : 0) + (rw - SW) / 2, SA_Top, ((ntsc_max || wide_max) ? rect.MinY : 0) + (rh - SH) / 2,
		SA_Quiet, TRUE, SA_ShowTitle, FALSE, SA_Title, (ULONG)"Arnold",
		SA_Interleaved, TRUE,
		TAG_DONE);
	if (!scr) { Sys_Log("display: OpenScreen failed"); return 0; }
	Sys_Log("display: screen left %d top %d size %dx%d, viewport DxOffset %d DyOffset %d DWidth %d DHeight %d", (int)scr->LeftEdge, (int)scr->TopEdge, (int)scr->Width, (int)scr->Height,
		(int)scr->ViewPort.DxOffset, (int)scr->ViewPort.DyOffset, (int)scr->ViewPort.DWidth, (int)scr->ViewPort.DHeight);

	safeport = CreateMsgPort(); dispport = CreateMsgPort();
	if (!safeport || !dispport) { Sys_CloseDisplay(); return 0; }
	Sys_VShiftOn = 0; TR = SH;
	for (i = 0; i < NBUF; i++) { win_row[i] = 0; sb[i] = NULL; own_bm[i] = NULL; safe_pend[i] = disp_pend[i] = 0; }
#ifdef VSHIFT
	/* vertical shift: three tall chip areas and window bitmaps; anything
	 * missing (chip RAM) falls back to the plain buffers */
	sb_scr = NULL;
	for (i = 0; i < NBUF; i++) tall_bm[i] = NULL;
	if (!Sys_NoVShift) {
		struct BitMap *sbm = scr->RastPort.BitMap;
		ULONG fl = BMF_DISPLAYABLE | BMF_CLEAR | (GetBitMapAttr(sbm, BMA_FLAGS) & BMF_INTERLEAVED);
		int ok = 1;
		for (i = 0; i < NBUF && ok; i++) {
			tall_bm[i] = AllocBitMap(GetBitMapAttr(sbm, BMA_WIDTH), SH + 2 * VS_MARGIN, GetBitMapAttr(sbm, BMA_DEPTH), fl, NULL);
			if (!tall_bm[i] || tall_bm[i]->Depth < 8) ok = 0;
		}
		if (ok) {
			sb_scr = AllocScreenBuffer(scr, NULL, SB_SCREEN_BITMAP);
			if (!sb_scr) ok = 0;
		}
		/* OS 3.2 refuses a bitmap of our own (see below), so Intuition allocates
		 * the buffers and their plane pointers are aimed into the tall areas;
		 * the originals are put back before FreeScreenBuffer */
		for (i = 0; i < NBUF && ok; i++) {
			struct BitMap *b;
			int p;
			sb[i] = AllocScreenBuffer(scr, NULL, 0);
			if (!sb[i]) { ok = 0; break; }
			b = sb[i]->sb_BitMap;
			for (p = 0; p < 8; p++) sb_planes0[i][p] = b->Planes[p];
			if (b->Depth < 8 || b->BytesPerRow != tall_bm[i]->BytesPerRow
			 || b->Planes[1] - b->Planes[0] != tall_bm[i]->Planes[1] - tall_bm[i]->Planes[0]) {
				Sys_Log("display: buffer %d layout bpr %d +%ld differs from the tall area's bpr %d +%ld", i, (int)b->BytesPerRow,
					(long)(b->Planes[1] - b->Planes[0]), (int)tall_bm[i]->BytesPerRow, (long)(tall_bm[i]->Planes[1] - tall_bm[i]->Planes[0]));
				ok = 0; break;
			}
			vs_place(i, VS_MARGIN);
		}
		if (ok) {
			Sys_VShiftOn = 1; TR = SH + 2 * VS_MARGIN;
			sb_scr->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = safeport;
			sb_scr->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = dispport;
			Sys_Log("display: vertical shift on, %d rows a buffer, bpr %d, chip free %lu", TR, (int)tall_bm[0]->BytesPerRow, (unsigned long)AvailMem(MEMF_CHIP));
		} else {
			Sys_Log("display: vertical shift off (chip RAM or screen buffers), chip free %lu", (unsigned long)AvailMem(MEMF_CHIP));
			for (i = 0; i < NBUF; i++) {
				if (sb[i]) { int p; for (p = 0; p < 8; p++) sb[i]->sb_BitMap->Planes[p] = sb_planes0[i][p]; FreeScreenBuffer(scr, sb[i]); sb[i] = NULL; }
				win_row[i] = 0;
			}
			if (sb_scr) { FreeScreenBuffer(scr, sb_scr); sb_scr = NULL; }
			WaitBlit();
			for (i = 0; i < NBUF; i++) if (tall_bm[i]) { FreeBitMap(tall_bm[i]); tall_bm[i] = NULL; }
		}
	}
#endif
	Sys_VShiftOn_Last = Sys_VShiftOn;
	for (i = 0; i < NBUF; i++) {
		struct BitMap *sbm = scr->RastPort.BitMap;
		if (Sys_VShiftOn) goto have_sb;
		if (i > 0) {
			/* 15 Oct 2026: on the A1200 with P96 installed, the system's buffers may
			 * not match the screen bitmap's layout (interleaved or not), which shows
			 * as colour soup in frames from those buffers. Make them ourselves as an
			 * exact friend of the screen bitmap. */
			ULONG fl = BMF_DISPLAYABLE | BMF_CLEAR | (GetBitMapAttr(sbm, BMA_FLAGS) & BMF_INTERLEAVED);
			/* no friend bitmap: with P96 installed a friend may be taken as an RTG request */
			own_bm[i] = AllocBitMap(GetBitMapAttr(sbm, BMA_WIDTH), GetBitMapAttr(sbm, BMA_HEIGHT),
				GetBitMapAttr(sbm, BMA_DEPTH), fl, NULL);
			if (own_bm[i])
				Sys_Log("display: own bitmap %d bpr %d planes +%ld flags %08lx", i, (int)own_bm[i]->BytesPerRow,
					(long)(own_bm[i]->Planes[1] - own_bm[i]->Planes[0]), (unsigned long)GetBitMapAttr(own_bm[i], BMA_FLAGS));
		}
		sb[i] = AllocScreenBuffer(scr, own_bm[i], i == 0 ? SB_SCREEN_BITMAP : 0);
		if (!sb[i] && own_bm[i]) {
			/* refused: fall back to the system's buffer (logged below) */
			Sys_Log("display: AllocScreenBuffer refused own bitmap %d, using the system's", i);
			FreeBitMap(own_bm[i]); own_bm[i] = NULL;
			sb[i] = AllocScreenBuffer(scr, NULL, 0);
		}
		if (!sb[i]) { Sys_Log("display: AllocScreenBuffer %d failed", i); Sys_CloseDisplay(); return 0; }
	have_sb:
		{
			struct BitMap *b = sb[i]->sb_BitMap;
			Sys_Log("display: buffer %d %s bpr %d planes %08lx %08lx (+%ld) flags %08lx", i, Sys_VShiftOn ? "window" : own_bm[i] ? "own" : "system",
				(int)b->BytesPerRow, (unsigned long)b->Planes[0], (unsigned long)b->Planes[1],
				(long)(b->Planes[1] - b->Planes[0]), (unsigned long)GetBitMapAttr(b, BMA_FLAGS));
		}
		sb[i]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = safeport;
		sb[i]->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = dispport;
	}
	line_dirty = AllocMem(CH, MEMF_ANY);
	if (!line_dirty) { Sys_CloseDisplay(); return 0; }
	memset(line_dirty, ALL_BUFFERS, CH);   /* full first conversion into every buffer */
	for (i = 0; i < NBUF; i++) {
		line_lvl[i] = AllocVec(TR, MEMF_ANY);
		if (!line_lvl[i]) { Sys_CloseDisplay(); return 0; }
		memset(line_lvl[i], 8, TR);        /* unknown: the first narrow conversion clears the rest */
#ifndef NO_SHADOW
		memset(line_lvl[i], 0, TR);        /* shadow not known yet */
		shadow[i] = AllocVec((ULONG)TR * SW, MEMF_ANY);   /* 8 planes x SW/8 bytes a row */
		if (!shadow[i]) { Sys_CloseDisplay(); return 0; }
#endif
	}
#ifdef VSHIFT
	if (Sys_VShiftOn) {
		chash = AllocVec((ULONG)CH * 4, MEMF_ANY);
		chash_stale = AllocVec(CH, MEMF_ANY);
		if (!chash || !chash_stale) { Sys_CloseDisplay(); return 0; }
		memset(chash_stale, 1, CH);
		for (i = 0; i < NBUF; i++) {
			row_hash[i] = AllocVec((ULONG)TR * 4, MEMF_ANY | MEMF_CLEAR);
			if (!row_hash[i]) { Sys_CloseDisplay(); return 0; }
		}
	}
#endif
	for (i = 0; i < NBUF; i++) buf_safe[i] = Sys_VShiftOn || (i != 0);   /* vshift: the screen's own bitmap is on show, not buffer 0 */
	shown = 0; safe_out = disp_out = 0;

	win = OpenWindowTags(NULL,
		WA_CustomScreen, (ULONG)scr, WA_Left, 0, WA_Top, 0,
		WA_Width, SW, WA_Height, SH,
		WA_Backdrop, TRUE, WA_Borderless, TRUE, WA_Activate, TRUE,
		WA_RMBTrap, TRUE, WA_SimpleRefresh, TRUE, WA_NoCareRefresh, TRUE,
		/* EXTENDEDMOUSE: take wheel events ourselves (and drop them); without it
		 * Intuition turns wheel movement into cursor up/down key presses. */
		WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS | IDCMP_EXTENDEDMOUSE,
		TAG_DONE);
	if (!win) { Sys_Log("display: OpenWindow failed"); Sys_CloseDisplay(); return 0; }
	blankptr = AllocVec(16, MEMF_CHIP | MEMF_CLEAR);
	if (blankptr) SetPointer(win, blankptr, 1, 1, 0, 0);

	pal32[0] = 256UL << 16;
	pal32[1 + 256 * 3] = 0;
	pal32[1 + SYS_OSD_PEN * 3] = 0xFFFFFFFF; pal32[2 + SYS_OSD_PEN * 3] = 0xDDDDDDDD; pal32[3 + SYS_OSD_PEN * 3] = 0;   /* OSD label: yellow */
	pal_dirty = 1; pal_dirty_all = 1;   /* new screen: load every pen once */
	measure_vblank(modeid);
	Sys_Log("display: open");
	return 1;
}

static void collect_safe(void)
{
	struct Message *m;
	int j;
	if (!safeport) return;   /* RTG screen (no AGA buffers) or no display */
	while ((m = GetMsg(safeport))) {
		for (j = 0; j < NBUF; j++)
			if (sb[j] && m == &sb[j]->sb_DBufInfo->dbi_SafeMessage) { buf_safe[safe_for[j]] = 1; safe_pend[j] = 0; }
		safe_out--;
	}
}

static void collect_disp(void)
{
	struct Message *m;
	int j;
	if (!dispport) return;   /* RTG screen: Sys_WaitFrame calls this every frame, and GetMsg(NULL) reads low memory */
	while ((m = GetMsg(dispport))) {
		for (j = 0; j < NBUF; j++)
			if (sb[j] && m == &sb[j]->sb_DBufInfo->dbi_DispMessage) disp_pend[j] = 0;
		disp_out--;
	}
}

void Sys_CloseDisplay(void)
{
	int i;
	if (Sys_RtgActive) { rtg_close_display(); return; }
	while (safe_out > 0) { WaitPort(safeport); collect_safe(); }
	while (disp_out > 0) { WaitPort(dispport); collect_disp(); }
#ifdef VSHIFT
	/* the screen's own bitmap back on show before the windows go */
	if (sb_scr && scr && ChangeScreenBuffer(scr, sb_scr)) {
		safe_out++; disp_out++;
		while (safe_out > 0) { WaitPort(safeport); collect_safe(); }
		while (disp_out > 0) { WaitPort(dispport); collect_disp(); }
	}
#endif
	/* Plain buffers (NOVSHIFT, or chip RAM too short for the tall areas): sb[0] is
	 * the screen's own bitmap. Put it back on show too. CloseScreen frees the
	 * bitmap that is installed (intuition.doc, AllocScreenBuffer NOTES), so with
	 * buffer 1 or 2 on show it freed that one, and an own_bm[] bitmap (where the
	 * OS accepts them) was then freed a second time below. As the vshift path. */
	if (!Sys_VShiftOn && flipped && shown != 0 && sb[0] && scr && ChangeScreenBuffer(scr, sb[0])) {
		safe_out++; disp_out++;
		while (safe_out > 0) { WaitPort(safeport); collect_safe(); }
		while (disp_out > 0) { WaitPort(dispport); collect_disp(); }
	}
	flipped = 0;
	if (win) { ClearPointer(win); CloseWindow(win); win = NULL; }
	if (blankptr) { FreeVec(blankptr); blankptr = NULL; }
	for (i = 0; i < NBUF; i++) {
#ifdef VSHIFT
		if (sb[i] && Sys_VShiftOn) { int p; for (p = 0; p < 8; p++) sb[i]->sb_BitMap->Planes[p] = sb_planes0[i][p]; }
#endif
		if (sb[i] && scr) { FreeScreenBuffer(scr, sb[i]); sb[i] = NULL; }
		if (own_bm[i]) { WaitBlit(); FreeBitMap(own_bm[i]); own_bm[i] = NULL; }
	}
#ifdef VSHIFT
	if (sb_scr && scr) { FreeScreenBuffer(scr, sb_scr); sb_scr = NULL; }
	for (i = 0; i < NBUF; i++) {
		if (tall_bm[i]) { WaitBlit(); FreeBitMap(tall_bm[i]); tall_bm[i] = NULL; }
		if (row_hash[i]) { FreeVec(row_hash[i]); row_hash[i] = NULL; }
	}
	if (chash) { FreeVec(chash); chash = NULL; }
	if (chash_stale) { FreeVec(chash_stale); chash_stale = NULL; }
	Sys_VShiftOn = 0;
#endif
	if (line_dirty) { FreeMem(line_dirty, CH); line_dirty = NULL; }
	for (i = 0; i < NBUF; i++) if (line_lvl[i]) { FreeVec(line_lvl[i]); line_lvl[i] = NULL; }
	for (i = 0; i < NBUF; i++) if (shadow[i]) { FreeVec(shadow[i]); shadow[i] = NULL; }
	if (scr) { CloseScreen(scr); scr = NULL; Sys_Log("display: closed"); }
	if (safeport) { DeleteMsgPort(safeport); safeport = NULL; }
	if (dispport) { DeleteMsgPort(dispport); dispport = NULL; }
}

void Sys_SetPaletteEntry(int index, unsigned char r, unsigned char g, unsigned char b)
{
	/* The core pushes every pen each frame (Palette_Set); only colours that really
	 * change are marked. Reloading all 256 every frame cost ~15 ms a frame on
	 * BigMig (13 Oct 2026: Head Over Heels spent 15 ms "in c2p" converting 1 line). */
	ULONG *e = &pal32[1 + index * 3];
	ULONG nr = (ULONG)r * 0x01010101UL, ng = (ULONG)g * 0x01010101UL, nb = (ULONG)b * 0x01010101UL;
	if (index < 0 || index > 255) return;
	if (e[0] == nr && e[1] == ng && e[2] == nb && !pal_dirty_all) return;
	e[0] = nr; e[1] = ng; e[2] = nb;
	if (!pal_dirty) { pal_lo = pal_hi = index; pal_dirty = 1; }
	else { if (index < pal_lo) pal_lo = index; if (index > pal_hi) pal_hi = index; }
}

/* LoadRGB32 for the changed range only: a table that starts with (count<<16 |
 * first) and the colours from pal32. */
static void load_palette(void)
{
	static ULONG tab[1 + 256 * 3 + 1];
	int n;
	if (pal_dirty_all) { pal_lo = 0; pal_hi = 255; pal_dirty_all = 0; }
	n = pal_hi - pal_lo + 1;
	tab[0] = ((ULONG)n << 16) | pal_lo;
	CopyMem(&pal32[1 + pal_lo * 3], &tab[1], n * 3 * sizeof(ULONG));
	tab[1 + n * 3] = 0;
	LoadRGB32(&scr->ViewPort, tab);
	pal_dirty = 0;
	Sys_Perf.palette_loads++;
}

/* ---- performance counters (PERF) ---- */
struct Device *TimerBase;
static struct timerequest timereq;
static ULONG eclock_hz;
SysPerf Sys_Perf;

unsigned long Sys_Micros(void)
{
	struct EClockVal ev;
	if (!TimerBase) {
		if (OpenDevice((STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&timereq, 0) != 0) return 0;
		TimerBase = timereq.tr_node.io_Device;
	}
	eclock_hz = ReadEClock(&ev);
	/* 64-bit ticks -> microseconds (low 32 bits: wraps after 71 minutes) */
	{
		unsigned long long t = ((unsigned long long)ev.ev_hi << 32) | ev.ev_lo;
		return (unsigned long)(t * 1000000ULL / eclock_hz);
	}
}

void Sys_MarkLineDirty(int y)
{
	if (line_dirty && y >= 0 && y < CH) {
		line_dirty[y] = ALL_BUFFERS;
#ifdef VSHIFT
		if (chash_stale) chash_stale[y] = 1;
#endif
	}
}

void Sys_ShowFrame(const unsigned char *chunky, int pitch)
{
	unsigned long t0, t1;
	struct BitMap *bm;
	int y, next, bit, wr, moved;

	if (Sys_RtgActive) { rtg_show(chunky, pitch); return; }
	if (!scr) return;
	t0 = Sys_Micros();
	next = (shown + 1) % NBUF;
	collect_safe();
	if (Sys_Audit) { collect_disp(); audit_copper(); }
	if (!buf_safe[next]) {
		hb_safe_waits++;
		if (hb_log_waits) Sys_Log("wait: buffer %d not safe yet (shown %d, safe_out %d, disp_out %d)", next, shown, safe_out, disp_out);
	}
	while (!buf_safe[next]) { WaitPort(safeport); collect_safe(); }
	t1 = Sys_Micros(); Sys_Perf.wait_us += t1 - t0; Sys_Perf.wait_safe_us += t1 - t0; t0 = t1;

	bit = 1 << next;
	wr = win_row[next];
	moved = 0;
#ifdef VSHIFT
	if (Sys_VShiftOn) {
		int nd = 0, r;
		for (y = 0; y < SH; y++) {
			if (chash_stale[y + YOFF]) { chash[y + YOFF] = hash_line(chunky + (y + YOFF) * pitch + XOFF); chash_stale[y + YOFF] = 0; }
			nd += (line_dirty[y + YOFF] & bit) != 0;
		}
		r = vs_choose(next, Sys_AllLines ? SH : nd);
		if (r != wr) { vs_place(next, r); wr = r; moved = 1; }
		t1 = Sys_Micros(); Sys_Perf.vs_us += t1 - t0; t0 = t1;
	}
#endif
	bm = sb[next]->sb_BitMap;
	for (y = 0; y < SH; y++) {
		const unsigned char *src = chunky + (y + YOFF) * pitch + XOFF;
		if (moved || Sys_AllLines || (line_dirty[y + YOFF] & bit)) {
			line_dirty[y + YOFF] &= ~bit;
#ifndef NO_SHADOW
			{
				int np = line_planes(src), t = wr + y;
				c2p_line_sh(src, bm, y, np, shadow[next] + (LONG)t * (SW >> 2), line_lvl[next][t]);
				line_lvl[next][t] = np;
#ifdef VSHIFT
				if (Sys_VShiftOn) row_hash[next][t] = chash[y + YOFF];
#endif
				if (np == 5) Sys_Perf.c2p_lines5++; else if (np == 6) Sys_Perf.c2p_lines6++;
			}
#elif !defined(NO_C2P5)
			{
				int np = line_planes(src), old = line_lvl[next][y];
				if (np < 8) {
					c2p_line_n(src, bm, y, np, old);
					if (np == 5) Sys_Perf.c2p_lines5++; else Sys_Perf.c2p_lines6++;
					if (old > np) Sys_Perf.c2p_clear_planes += old - np;
				} else c2p_line(src, bm, y);
				line_lvl[next][y] = np;
			}
#else
			c2p_line(src, bm, y);
#endif
			Sys_Perf.c2p_lines++;
		}
	}
	if (Sys_Audit) audit_frame(chunky, pitch, bm, next);
	if (Sys_Osd) {
		draw_osd(bm);
#ifndef NO_SHADOW
		for (y = 0; y < 12 && y < SH; y++) line_lvl[next][wr + y] = 0;   /* the label is not in the shadow */
#else
		for (y = 0; y < 12 && y < SH; y++) line_lvl[next][y] = 8;   /* the label's pen is 255 */
#endif
	}
	t1 = Sys_Micros(); Sys_Perf.c2p_us += t1 - t0; t0 = t1;
	if (Sys_PalAll) pal_dirty = pal_dirty_all = 1;   /* PALALL: diagnostic, all 256 every frame */
	if (pal_dirty) load_palette();
	t1 = Sys_Micros(); Sys_Perf.pal_us += t1 - t0; t0 = t1;
	Sys_Perf.c2p_frames++;
	Sys_Perf.line_bytes = SW;                 /* 8 planes x SW/8 bytes = SW bytes of chip RAM a line */

	/* No wait for the previous flip's DispMessage: in OS 3.2 it comes two VBlanks
	 * after ChangeScreenBuffer, which held every frame to 25 fps (13 Oct 2026).
	 * Pacing is WaitTOF in Sys_WaitFrame; the third buffer covers the late
	 * SafeMessages. If the change is refused, this frame is simply not shown. */
	collect_disp();
	if (Sys_SafeFlip) {
		/* diagnostic: a plain flip, every message of the last change answered first */
		int j;
		for (j = 0; j < NBUF; j++)
			while (safe_pend[j] || disp_pend[j]) {
				if (safe_pend[j]) { WaitPort(safeport); collect_safe(); }
				else { WaitPort(dispport); collect_disp(); }
			}
	}
	while (safe_pend[next] || disp_pend[next]) {
		hb_reuse_waits++;
		if (safe_pend[next]) { WaitPort(safeport); collect_safe(); }
		else { WaitPort(dispport); collect_disp(); }
	}
	if (!ChangeScreenBuffer(scr, sb[next])) hb_refused++;
	else {
		safe_for[next] = shown;          /* this change's SafeMessage frees the old one */
		buf_safe[shown] = 0;
		buf_safe[next] = 0;
		shown = next;
		flipped = 1;
		safe_out++; disp_out++;
		safe_pend[next] = disp_pend[next] = 1;
	}
	frames++;
	t1 = Sys_Micros(); Sys_Perf.wait_us += t1 - t0; Sys_Perf.wait_disp_us += t1 - t0;
}

/* Pacing (19 Oct 2026). Sys_SetPacing(timer, frame_us):
 * - timer 0: one CPC frame per VBlank, as before (tuned on PAL; on a 60 Hz
 *   display this is "Follow display": the game runs 20% fast);
 * - timer 1: the CPC's 50 Hz from the E-clock, whatever the display: each
 *   frame is due frame_us after the last one, and ChangeScreenBuffer shows it
 *   at the next VBlank (on 60 Hz, one frame in five is shown twice).
 * frame_us is the length of a sound block, so Paula and the emulation agree. */
static int pace_timer;
static unsigned long pace_us = 20000;
static struct MsgPort *delayport;
static struct timerequest *delayreq;

void Sys_SetPacing(int timer, unsigned long frame_us)
{
	pace_timer = timer;
	pace_us = frame_us;
	Sys_Log("pacing: %s, %lu us a frame", timer ? "E-clock timer (50 Hz whatever the display)" : "display VBlank", frame_us);
}

static void timer_delay(unsigned long us)
{
	if (!delayreq) {
		delayport = CreateMsgPort();
		if (delayport) delayreq = (struct timerequest *)CreateIORequest(delayport, sizeof(struct timerequest));
		if (delayreq && OpenDevice((STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)delayreq, 0) != 0) {
			DeleteIORequest((struct IORequest *)delayreq); delayreq = NULL;
		}
		if (!delayreq) { Sys_Log("pacing: no timer.device MICROHZ, using WaitTOF"); pace_timer = 0; WaitTOF(); return; }
	}
	delayreq->tr_node.io_Command = TR_ADDREQUEST;
	delayreq->tr_time.tv_secs = us / 1000000;
	delayreq->tr_time.tv_micro = us % 1000000;
	DoIO((struct IORequest *)delayreq);
}

static void timer_close(void)
{
	if (delayreq) { CloseDevice((struct IORequest *)delayreq); DeleteIORequest((struct IORequest *)delayreq); delayreq = NULL; }
	if (delayport) { DeleteMsgPort(delayport); delayport = NULL; }
}

long Sys_WaitFrame(void)
{
	/* VBlank mode: one CPC frame per VBlank. On time: WaitTOF, and the next
	 * frame is due 20 ms (pace_us) after that VBlank. Late (13 Oct 2026: BigMig
	 * took 40-80 ms a frame): no wait, and the schedule is kept, so the debt
	 * shows how far behind the emulation is; the frame skip uses it to catch
	 * up. More than 10 frames behind: give up the debt and restart the
	 * schedule from now. Timer mode: the same, but the wait is for the due
	 * time itself and the schedule is never re-locked to a VBlank.
	 * Returns how far behind schedule this frame ended, in us (0 if on time). */
	static unsigned long due;
	static int was_timer;
	long behind = 0;
	unsigned long t0 = Sys_Micros();
	collect_disp();
	if (pace_timer) {
		long d;
		if (!was_timer || due == 0 || (long)(t0 - due) > 1000000 || (long)(due - t0) > (long)(2 * pace_us)) due = t0 + pace_us;   /* (re)start */
		was_timer = 1;
		d = (long)(due - t0);
		if (d > 500) {
			timer_delay((unsigned long)d);
			due += pace_us;
		} else {
			behind = (long)(t0 - due);
			if (behind > 200000) due = t0 + pace_us;
			else due += pace_us;
			if (behind > 0) Sys_Perf.late_frames++;
		}
	} else {
		was_timer = 0;
		if (due == 0 || (long)(due - t0) > 500) {
			WaitTOF();
			due = Sys_Micros() + pace_us;
		} else {
			behind = (long)(t0 - due);
			if (behind > 200000) due = t0 + pace_us;   /* still reported: the skip carries on */
			else due += pace_us;
			Sys_Perf.late_frames++;
		}
	}
	t0 = Sys_Micros() - t0;
	Sys_Perf.wait_us += t0; Sys_Perf.wait_throttle_us += t0; Sys_Perf.throttle_calls++;
	return behind > 0 ? behind : 0;
}

/* ---- input ---- */

int Sys_Poll(void)
{
	struct IntuiMessage *msg;
	if (!win) return 1;
	while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort))) {
		/* 0x78+: mouse wheel and buttons, not keys. A held key's auto-repeat events (a
		 * second "down" for the same key) are dropped here: the key stays down until its
		 * key-up, so a held fire or direction can't be disturbed by them and they can't
		 * fill the queue while the emulation is stopped. A key-up is never lost: if the
		 * queue is full, the oldest "down" makes room for it (26 Oct 2026). */
		if (msg->Class == IDCMP_RAWKEY && (msg->Code & 0x7f) < 0x78 && !(msg->Qualifier & IEQUALIFIER_REPEAT)) {
			int next = (kq_tail + 1) % KQ;
			if (next == kq_head && (msg->Code & 0x80)) {
				int i;
				for (i = kq_head; i != kq_tail; i = (i + 1) % KQ) if (!(keyq[i] & 0x80)) break;
				if (i == kq_tail) i = kq_head;   /* all key-ups: the oldest goes */
				for (; i != kq_head; i = (i + KQ - 1) % KQ) { int p = (i + KQ - 1) % KQ; keyq[i] = keyq[p]; keyqual[i] = keyqual[p]; }
				kq_head = (kq_head + 1) % KQ;
			}
			if (next != kq_head) { keyq[kq_tail] = msg->Code; keyqual[kq_tail] = msg->Qualifier; kq_tail = next; }
		}
		ReplyMsg((struct Message *)msg);
	}
	return quit_requested;
}

void Sys_RequestQuit(void) { quit_requested = 1; }

int Sys_GetKey(int *rawcode, int *down, int *qualifier)
{
	if (kq_head == kq_tail) return 0;
	*rawcode = keyq[kq_head] & 0x7f;
	*down = !(keyq[kq_head] & 0x80);
	*qualifier = keyqual[kq_head];
	kq_head = (kq_head + 1) % KQ;
	return 1;
}

/* Joystick: the hardware port 2 (joy1dat, CIA-A fire, POTINP fire 2) as
 * AmiMSX reads it, OR'd with lowlevel.library's view of ports 0-3 (14 Oct
 * 2026: a USB pad under Poseidon only shows up through lowlevel.library's
 * joystick-port emulation, not in the hardware registers). Port 0 is skipped
 * while it holds the mouse. Red = fire 1, blue (or any other button) = fire 2. */
#include <libraries/lowlevel.h>
#include <proto/lowlevel.h>
struct Library *LowLevelBase;
static int lowlevel_tried;

static ULONG joy_last_raw[4];
ULONG Sys_JoyRaw(int port) { return port >= 0 && port < 4 ? joy_last_raw[port] : 0; }

/* CD32 pad on port 2, read by us (15 Oct 2026). On the A1200 PiStorm,
 * lowlevel.library saw John's KTRL CD32 pad as a game controller but only
 * ever reported the red button: on a CPU that fast its shift-register reads
 * outrun the pad. This is the usual routine games use (cf. Codetapper's
 * CD32 pad notes): pin 5 low selects CD32 mode, pin 6 (fire) becomes the
 * clock, and the 7 buttons come out one by one on pin 9 (POTINP bit 14,
 * low = pressed). Each step waits on CIA reads, which run at real Amiga
 * speed whatever the CPU. Order: blue, red, yellow, green, forward, reverse,
 * play. Returns the 7 button bits (bit set = pressed). */
static unsigned read_cd32_buttons(void)
{
	/* Returns 9 bits, set = low on pin 9: the 7 buttons, then the pad's ID
	 * bits (a CD32 pad gives 8th = 0, 9th = 1 here). As AmiMSX does it
	 * (John's decision, 17 Oct 2026): pin 6 is driven HIGH as an output
	 * before pin 5 falls, otherwise a held red button gives a spurious clock;
	 * the edges are timed with CIA reads, which run at real Amiga speed
	 * whatever the CPU. Order: blue, red, yellow, green, forward, reverse,
	 * play. */
	volatile UBYTE *pra = &ciaa.ciapra, *ddra = &ciaa.ciaddra;
	unsigned bits = 0;
	int i, d;
	UBYTE dummy;
	Disable();
	*pra |= 0x80;                  /* clock high first... */
	*ddra |= 0x80;                 /* ...then port 2 fire line as an output */
	for (d = 0; d < 4; d++) dummy = *pra;
	custom.potgo = 0x6f00;         /* port 2: pin 5 output low (CD32 mode), pin 9 input */
	for (d = 0; d < 25; d++) dummy = *pra;
	for (i = 0; i < 9; i++) {
		for (d = 0; d < 10; d++) dummy = *pra;
		if (!(custom.potinp & 0x4000)) bits |= 1u << i;
		*pra &= ~0x80;             /* clock pulse: next bit */
		for (d = 0; d < 4; d++) dummy = *pra;
		*pra |= 0x80;
	}
	*ddra &= ~0x80;                /* fire line back to input */
	custom.potgo = 0xff00;         /* pin 5 high again: 2-button mode */
	Enable();
	(void)dummy;
	return bits;
}

/* Joystick port 2 (lowlevel.library port 1). No auto-detection (John's
 * decision, 17 Oct 2026, after AmiMSX: lowlevel.library's CD32 detection was
 * wrong for his adapters and even for a plain joystick, and a plain joystick
 * read as a CD32 pad has its fire line driven as a clock, so nothing works).
 * - Sys_PadType 0, standard: the hardware registers as AmiMSX reads them
 *   (joy1dat, CIA-A fire, POTINP fire 2 on pin 9 or pin 5). Pin 6 is never
 *   driven, so a plain joystick sees no probe at all.
 * - Sys_PadType 1, CD32 pad: directions from joy1dat, the buttons from
 *   read_cd32_buttons(), no ID check. Red = fire 1; blue, green, yellow = fire 2.
 * Game controllers lowlevel.library reports on ports 2-3 (USB pads under
 * Poseidon) are OR'd in either way. */
int Sys_PadType;                    /* 0 standard, 1 CD32 pad (menu / PAD= / prefs pad=) */
int Sys_NoCD32;                     /* NOCD32: kept for old scripts, same as PAD=0 */
static int pad_logged = -1;
unsigned Sys_ReadJoystick(void)
{
	unsigned m = 0;
	int port;
	UWORD j = custom.joy1dat;
	if (!lowlevel_tried) {
		lowlevel_tried = 1;
		LowLevelBase = OpenLibrary((STRPTR)"lowlevel.library", 39);
		Sys_Log("joystick: lowlevel.library %s", LowLevelBase ? "open (USB pads on ports 2-3)" : "not available");
	}
	if (Sys_NoCD32) Sys_PadType = 0;
	if (pad_logged != Sys_PadType) {
		pad_logged = Sys_PadType;
		Sys_Log("joystick: port 2 read as %s", Sys_PadType ? "a CD32 pad" : "a standard 2-button joystick");
	}
	if (j & 0x0002) m |= SYS_JOY_RIGHT;
	if (j & 0x0200) m |= SYS_JOY_LEFT;
	if (((j >> 1) ^ j) & 0x0001) m |= SYS_JOY_DOWN;
	if (((j >> 9) ^ (j >> 8)) & 0x0001) m |= SYS_JOY_UP;
	if (Sys_PadType) {
		unsigned b = read_cd32_buttons();
		joy_last_raw[1] = b;
		if (b & 0x02) m |= SYS_JOY_FIRE1;                    /* red */
		if (b & (0x01 | 0x04 | 0x08)) m |= SYS_JOY_FIRE2;   /* blue, yellow, green */
	} else {
		if (!(ciaa.ciapra & 0x80)) m |= SYS_JOY_FIRE1;
		custom.potgo = 0xff00;
		/* fire 2 on pin 9 (Amiga standard) or pin 5 (as AmiMSX reads it; some 2-button pads) */
		if (!(custom.potinp & 0x4000) || !(custom.potinp & 0x1000)) m |= SYS_JOY_FIRE2;
	}
	if (LowLevelBase) {
		for (port = 2; port < 4; port++) {
			ULONG v = ReadJoyPort(port);
			ULONG type = v & JP_TYPE_MASK;
			if (v != joy_last_raw[port]) joy_last_raw[port] = v;
			if (type != JP_TYPE_GAMECTLR && type != JP_TYPE_JOYSTK) continue;
			if (v & JPF_JOY_UP) m |= SYS_JOY_UP;
			if (v & JPF_JOY_DOWN) m |= SYS_JOY_DOWN;
			if (v & JPF_JOY_LEFT) m |= SYS_JOY_LEFT;
			if (v & JPF_JOY_RIGHT) m |= SYS_JOY_RIGHT;
			if (v & JPF_BUTTON_RED) m |= SYS_JOY_FIRE1;
			if (v & (JPF_BUTTON_BLUE | JPF_BUTTON_GREEN | JPF_BUTTON_YELLOW)) m |= SYS_JOY_FIRE2;
		}
	}
	return m;
}

/* ---- sound ---- */
/* Paula, channels 0 (left) and 1 (right), 8-bit signed (14 Oct 2026 rewrite).
 * The emulation makes one block a CPC frame (444 samples). A frame of the PAL
 * display is 313 x 227 = 71051 colour clocks; at period 160 a block lasts
 * 71040, so one block per VBlank keeps Paula fed at the display's pace. Blocks
 * go into a small queue; a VBlank interrupt server hands the oldest one to
 * Paula (it is latched when the playing block ends). No block ready (the
 * emulation is behind): Paula repeats the last one. Queue full (it caught up in
 * a burst): the oldest waiting block is dropped. Both are counted (Sys_Perf). */
#include <devices/audio.h>
#include <hardware/dmabits.h>
#include <hardware/intbits.h>
#include <exec/interrupts.h>
#include "audio_conv.h"
#define ABUFS 4                       /* playing + handed over + up to 2 queued */
static struct MsgPort *audport;
static struct IOAudio *audio;
static int aud_open, aud_n;
static UBYTE *aud_buf[2][ABUFS];      /* [channel][slot] in chip RAM */
static UBYTE aud_map[] = { 3 };       /* channels 0 and 1 */
static volatile int aud_rd, aud_wr, aud_count, aud_run;
static volatile unsigned long aud_underruns, aud_drops, aud_irqs;
static struct Interrupt aud_vbint;
static int aud_vbint_added;
/* 19 Oct 2026: on a display that isn't 50 Hz the VBlank can't pace Paula, so
 * the blocks are handed over by channel 0's own interrupt instead: Paula
 * raises it when it latches a block (the start of its playback), and the
 * pointer written then is latched when that block ends. The rate is then
 * Paula's own: samples x period colour clocks a block. aud_mode 0 = VBlank
 * server (PAL, as tuned), 1 = audio interrupt. */
static int aud_mode, aud_first;
static struct Interrupt aud_int, *aud_oldint;

static void aud_feed(void)
{
	if (aud_run) {
		if (aud_count > 0) {
			custom.aud[0].ac_ptr = (UWORD *)aud_buf[0][aud_rd];
			custom.aud[1].ac_ptr = (UWORD *)aud_buf[1][aud_rd];
			aud_rd = (aud_rd + 1) % ABUFS;
			aud_count--;
		} else if (aud_first)
			aud_first = 0;   /* the start: nothing queued behind the first block yet */
		else
			aud_underruns++;
	}
}

static LONG aud_vblank(register APTR data __asm("a1"))
{
	aud_feed();
	return 0;
}

/* exec interrupt handler (SetIntVector): must clear its request itself */
static void aud_handler(register APTR data __asm("a1"))
{
	custom.intreq = INTF_AUD0;
	aud_irqs++;
	aud_feed();
}

unsigned long Sys_AudioIrqs(void) { return aud_irqs; }

int Sys_AudioOpen(int period, int samples, int audint)
{
	int c, b;
	if (aud_open) return 1;
	aud_mode = audint;
	if (samples & 1) samples--;
	aud_n = samples;
	audport = CreateMsgPort();
	if (!audport) return 0;
	audio = (struct IOAudio *)CreateIORequest(audport, sizeof(struct IOAudio));
	if (!audio) { DeleteMsgPort(audport); audport = NULL; return 0; }
	audio->ioa_Request.io_Message.mn_Node.ln_Pri = 127;   /* don't let others steal them */
	audio->ioa_Request.io_Command = ADCMD_ALLOCATE;
	audio->ioa_Request.io_Flags = ADIOF_NOWAIT;
	audio->ioa_AllocKey = 0;
	audio->ioa_Data = aud_map;
	audio->ioa_Length = sizeof(aud_map);
	if (OpenDevice("audio.device", 0, (struct IORequest *)audio, 0) != 0) {
		Sys_Log("audio: audio.device channels 0+1 not free");
		DeleteIORequest((struct IORequest *)audio); audio = NULL;
		DeleteMsgPort(audport); audport = NULL;
		return 0;
	}
	aud_open = 1;
	for (c = 0; c < 2; c++) for (b = 0; b < ABUFS; b++) {
		aud_buf[c][b] = AllocMem(aud_n, MEMF_CHIP | MEMF_CLEAR);
		if (!aud_buf[c][b]) { Sys_Log("audio: no chip RAM for buffers"); Sys_AudioClose(); return 0; }
	}
	custom.aud[0].ac_per = period; custom.aud[1].ac_per = period;
	custom.aud[0].ac_vol = 64;     custom.aud[1].ac_vol = 64;
	custom.aud[0].ac_len = aud_n >> 1; custom.aud[1].ac_len = aud_n >> 1;
	aud_rd = aud_wr = aud_count = 0; aud_run = 0;
	if (aud_mode) {
		aud_int.is_Node.ln_Type = NT_INTERRUPT;
		aud_int.is_Node.ln_Pri = 0;
		aud_int.is_Node.ln_Name = (char *)"Arnold audio";
		aud_int.is_Data = NULL;
		aud_int.is_Code = (VOID (*)())aud_handler;
		custom.intena = INTF_AUD0;
		custom.intreq = INTF_AUD0;
		aud_oldint = SetIntVector(INTB_AUD0, &aud_int);
		custom.intena = INTF_SETCLR | INTF_AUD0;
	} else {
		aud_vbint.is_Node.ln_Type = NT_INTERRUPT;
		aud_vbint.is_Node.ln_Pri = 0;
		aud_vbint.is_Node.ln_Name = (char *)"Arnold audio";
		aud_vbint.is_Data = NULL;
		aud_vbint.is_Code = (VOID (*)())aud_vblank;
		AddIntServer(INTB_VERTB, &aud_vbint);
		aud_vbint_added = 1;
	}
	Sys_Log("audio: open, period %d, %d samples a block, fed by %s, chip clock %lu Hz", period, samples,
		aud_mode ? "the audio interrupt" : "the VBlank", Sys_ChipClock());
	return 1;
}

void Sys_AudioClose(void)
{
	int c, b;
	if (!aud_open) return;
	aud_run = 0;
	if (aud_vbint_added) { RemIntServer(INTB_VERTB, &aud_vbint); aud_vbint_added = 0; }
	custom.dmacon = DMAF_AUD0 | DMAF_AUD1;   /* clear */
	if (aud_mode) {
		custom.intena = INTF_AUD0;
		SetIntVector(INTB_AUD0, aud_oldint);
		custom.intreq = INTF_AUD0;
		aud_oldint = NULL; aud_mode = 0;
	}
	custom.aud[0].ac_vol = 0; custom.aud[1].ac_vol = 0;
	for (c = 0; c < 2; c++) for (b = 0; b < ABUFS; b++)
		if (aud_buf[c][b]) { FreeMem(aud_buf[c][b], aud_n); aud_buf[c][b] = NULL; }
	if (audio) { CloseDevice((struct IORequest *)audio); DeleteIORequest((struct IORequest *)audio); audio = NULL; }
	if (audport) { DeleteMsgPort(audport); audport = NULL; }
	aud_open = 0;
}

void Sys_AudioQueue(const unsigned char *s16, int samples)
{
	int slot;
	if (!aud_open) return;
	if (samples > aud_n) samples = aud_n;
	Disable();
	if (aud_count >= ABUFS - 2) {             /* full: drop the oldest waiting block */
		aud_rd = (aud_rd + 1) % ABUFS;
		aud_count--;
		aud_drops++;
	}
	slot = aud_wr;
	Enable();
	/* Slots in use: the block playing, the one handed over at the last VBlank
	 * (latched when the playing one ends) and the queue; with the queue kept
	 * below ABUFS - 2 the slot at aud_wr is none of them. */
	Audio_SplitTo8((const unsigned short *)s16, (signed char *)aud_buf[0][slot], (signed char *)aud_buf[1][slot], samples);
	if (samples < aud_n) {                    /* short block: hold the last level */
		memset(aud_buf[0][slot] + samples, samples ? aud_buf[0][slot][samples - 1] : 0, aud_n - samples);
		memset(aud_buf[1][slot] + samples, samples ? aud_buf[1][slot][samples - 1] : 0, aud_n - samples);
	}
#ifdef SOUNDQ_DUMP
	/* soundq verification builds only: every block as Paula gets it (left then
	   right, 8-bit signed) to PROGDIR:audio.raw, to compare with the headless WAV */
	{ static BPTR fh; static int tried;
	  if (!tried) { tried = 1; fh = Open((STRPTR)"PROGDIR:audio.raw", MODE_NEWFILE); }
	  if (fh) { Write(fh, aud_buf[0][slot], samples); Write(fh, aud_buf[1][slot], samples); Flush(fh); }
	  { static BPTR f16; static int tried16;   /* and the core's 16-bit block (big-endian) */
	    if (!tried16) { tried16 = 1; f16 = Open((STRPTR)"PROGDIR:audio16.raw", MODE_NEWFILE); }
	    if (f16) { Write(f16, (APTR)s16, samples * 4); Flush(f16); } } }
#endif
	Disable();
	aud_wr = (aud_wr + 1) % ABUFS;
	aud_count++;
	if (!aud_run) {
		/* start: this block plays at once, the server feeds the rest */
		custom.aud[0].ac_ptr = (UWORD *)aud_buf[0][slot];
		custom.aud[1].ac_ptr = (UWORD *)aud_buf[1][slot];
		aud_rd = aud_wr; aud_count = 0;
		aud_first = 1;
		if (aud_mode) custom.intreq = INTF_AUD0;
		custom.dmacon = DMAF_SETCLR | DMAF_MASTER | DMAF_AUD0 | DMAF_AUD1;
		if (aud_mode) custom.intena = INTF_SETCLR | INTF_AUD0;   /* off while paused, see Sys_AudioPause */
		aud_run = 1;
	}
	Enable();
	Sys_Perf.aud_underruns = aud_underruns;
	Sys_Perf.aud_drops = aud_drops;
}

void Sys_AudioPause(void)
{
	if (!aud_open) return;
	Disable();
	aud_run = 0;
	aud_rd = aud_wr = aud_count = 0;
	/* 19 Oct 2026 (NTSC menu shear on John's A1200): with the audio DMA off,
	 * an enabled AUD0 interrupt doesn't stop. Paula drops to manual mode and
	 * asks for a new word every sample period once the handler clears the
	 * request: about 22000 level-4 interrupts a second for as long as the
	 * menu was up (70000 counted in one WinUAE menu visit). They hold off the
	 * level-3 VBlank work of graphics and Intuition. So the interrupt is
	 * switched off here, and Sys_AudioQueue switches it on again with the DMA. */
	if (aud_mode) custom.intena = INTF_AUD0;
	custom.dmacon = DMAF_AUD0 | DMAF_AUD1;
	if (aud_mode) custom.intreq = INTF_AUD0;
	Enable();
}

unsigned long Sys_Millis(void) { return frames * 20; }
void Sys_TimerClose(void)
{
	timer_close();
	/* the E-clock request Sys_Micros opened (it was never closed) */
	if (TimerBase) { CloseDevice((struct IORequest *)&timereq); TimerBase = NULL; }
}

void Sys_Message(const char *text)
{
	Sys_Log("message: %s", text);
	struct EasyStruct es = { sizeof(struct EasyStruct), 0, "Arnold", "%s", "OK" };
	if (Sys_RtgActive) { EasyRequest(win, &es, NULL, (ULONG)text); return; }   /* on the RTG screen, which stays up */
	if (scr) Sys_CloseDisplay();
	EasyRequest(NULL, &es, NULL, (ULONG)text);
}

/* ---- BENCH: what is this machine fast or slow at? (13 Oct 2026) ---- */
static volatile unsigned long bench_sink;

static void bench_line(FILE *f, const char *name, unsigned long us, unsigned long units, const char *unit)
{
	/* rate as units per second, per microsecond x1000 to stay in 32 bits */
	unsigned long per_ms = us ? (unsigned long)((unsigned long long)units * 1000ULL / us) : 0;
	fprintf(f, "bench %-22s %8lu us  %8lu %s/ms\n", name, us, per_ms, unit);
	if (f != stdout) fflush(f);
}

/* Each memory region exec knows about, with a copy speed measured inside it:
 * does this machine have a faster kind of fast RAM than the one Arnold gets? */
static void bench_regions(FILE *f)
{
	struct MemHeader *mh;
	struct { char name[24]; ULONG lo, hi, attr, freeb; BYTE pri; } r[12];
	int n = 0, i;
	Forbid();
	for (mh = (struct MemHeader *)SysBase->MemList.lh_Head; mh->mh_Node.ln_Succ && n < 12;
	     mh = (struct MemHeader *)mh->mh_Node.ln_Succ, n++) {
		strncpy(r[n].name, mh->mh_Node.ln_Name ? mh->mh_Node.ln_Name : "?", 23); r[n].name[23] = 0;
		r[n].lo = (ULONG)mh->mh_Lower; r[n].hi = (ULONG)mh->mh_Upper;
		r[n].attr = mh->mh_Attributes; r[n].freeb = mh->mh_Free; r[n].pri = mh->mh_Node.ln_Pri;
	}
	Permit();
	for (i = 0; i < n; i++) {
		unsigned char *a = NULL, *b = NULL;
		unsigned long us = 0, t, p;
		/* two 64 KB blocks inside this region: AllocMem with the region's own
		 * attributes may land elsewhere, so check the address */
		if (r[i].freeb > 256 * 1024) {
			ULONG want = r[i].attr & (MEMF_CHIP | MEMF_FAST | MEMF_PUBLIC);
			a = AllocMem(65536, want); b = AllocMem(65536, want);
			if (a && b && (ULONG)a >= r[i].lo && (ULONG)a < r[i].hi && (ULONG)b >= r[i].lo && (ULONG)b < r[i].hi) {
				t = Sys_Micros();
				for (p = 0; p < 16; p++) memcpy(a, b, 65536);
				us = Sys_Micros() - t;
			}
			if (a) FreeMem(a, 65536);
			if (b) FreeMem(b, 65536);
		}
		fprintf(f, "region %-20s %08lx-%08lx pri %4d %s%s free %6lu KB  memcpy %s",
			r[i].name, r[i].lo, r[i].hi, r[i].pri,
			(r[i].attr & MEMF_CHIP) ? "CHIP " : "", (r[i].attr & MEMF_FAST) ? "FAST " : "",
			r[i].freeb / 1024, us ? "" : "n/a (not first in its class)\n");
		if (us) fprintf(f, "%lu KB/ms\n", (unsigned long)(16UL * 64UL * 1000UL / us));
	}
	{ /* where Arnold's own memory comes from */
		void *m = malloc(65536);
		fprintf(f, "region malloc() block at %08lx\n", (unsigned long)m);
		free(m);
	}
}

void Sys_Bench(FILE *f)
{
	bench_regions(f);
	enum { FB = 256 * 1024, CB = 64 * 1024 };
	unsigned char *a = AllocMem(FB, MEMF_ANY), *b = AllocMem(FB, MEMF_ANY);
	unsigned char *c = AllocMem(CB, MEMF_CHIP);
	unsigned long t, i, x = 1, p;
	if (!a || !b || !c) { fprintf(f, "bench: out of memory\n"); goto out; }
	memset(a, 1, FB); memset(b, 1, FB); memset(c, 0, CB);

	t = Sys_Micros();                                   /* integer ALU */
	for (i = 0; i < 4000000; i++) x = (x * 3) + ((x >> 5) ^ i);
	bench_sink = x; bench_line(f, "alu loop", Sys_Micros() - t, 4000000, "iter");

	t = Sys_Micros();                                   /* render-like lookup + byte store */
	for (i = 0; i < 2000000; i++) a[i & (FB - 1)] = (unsigned char)c2p_lo[b[(i * 7) & (FB - 1)]];
	bench_line(f, "lookup+byte store", Sys_Micros() - t, 2000000, "iter");

	t = Sys_Micros();                                   /* fast -> fast copy */
	for (p = 0; p < 8; p++) memcpy(a, b, FB);
	bench_line(f, "memcpy fast", Sys_Micros() - t, 8 * FB / 1024, "KB");

	t = Sys_Micros();                                   /* long compare, equal data */
	for (p = 0; p < 8; p++) {
		const unsigned long *la = (const unsigned long *)a, *lb = (const unsigned long *)b;
		for (i = 0; i < FB / 4; i++) if (la[i] != lb[i]) break;
		bench_sink += i;
	}
	bench_line(f, "long compare fast", Sys_Micros() - t, 8 * FB / 1024, "KB");

	t = Sys_Micros();                                   /* libnix memcmp */
	for (p = 0; p < 4; p++) bench_sink += memcmp(a, b, FB);
	bench_line(f, "memcmp (libnix)", Sys_Micros() - t, 4 * FB / 1024, "KB");

	t = Sys_Micros();                                   /* byte stores */
	for (p = 0; p < 4; p++) for (i = 0; i < FB; i++) a[i] = (unsigned char)i;
	bench_line(f, "byte store fast", Sys_Micros() - t, 4 * FB / 1024, "KB");

	t = Sys_Micros();                                   /* chip RAM long writes */
	for (p = 0; p < 8; p++) { unsigned long *lc = (unsigned long *)c; for (i = 0; i < CB / 4; i++) lc[i] = i; }
	bench_line(f, "chip long write", Sys_Micros() - t, 8 * CB / 1024, "KB");

	t = Sys_Micros();                                   /* chip RAM long reads */
	for (p = 0; p < 8; p++) { const unsigned long *lc = (const unsigned long *)c; for (i = 0; i < CB / 4; i++) x += lc[i]; }
	bench_sink = x; bench_line(f, "chip long read", Sys_Micros() - t, 8 * CB / 1024, "KB");

	t = Sys_Micros();                                   /* c2p of one 352-pixel line */
	{
		struct BitMap *bm = AllocBitMap(352, 2, 8, BMF_CLEAR, NULL);
		int saved = SW;
		if (bm) {
			SW = 352;
			t = Sys_Micros();
			for (p = 0; p < 200; p++) c2p_line(a, bm, (int)(p & 1));
			bench_line(f, "c2p line 352px (chip)", Sys_Micros() - t, 200, "line");
			SW = saved;
			FreeBitMap(bm);
		}
	}
out:
	if (a) FreeMem(a, FB);
	if (b) FreeMem(b, FB);
	if (c) FreeMem(c, CB);
}

void Sys_Heartbeat(long frame, long behind_us)
{
	Sys_Log("hb frame %ld late %lu skipped %lu behind_us %ld safe_waits %lu reuse_waits %lu refused %lu safe_out %d disp_out %d shown %d aud_q %d under %lu drops %lu",
		frame, Sys_Perf.late_frames, Sys_Perf.skipped_frames, behind_us, hb_safe_waits, hb_reuse_waits, hb_refused,
		safe_out, disp_out, shown, aud_count, aud_underruns, aud_drops);
	hb_log_waits = 1;   /* from now on, log each wait too (rare) */
}
