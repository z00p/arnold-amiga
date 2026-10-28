/* rtgfw.c - common RTG display framework, v0 (see rtgfw.h and ../DESIGN.md).
 *
 * Written fresh for this framework (MIT, see ../LICENSE); not derived from
 * Arnold's GPL code. Built with bebbo's amiga-gcc (m68k-amigaos, libnix).
 *
 * What this file does:
 *   rtg_query      Picasso96API.library 2+ / cybergraphics.library 41+, mode list
 *   rtg_open       mode choice, screen + backdrop window, 1-3 screen buffers
 *                  (AllocScreenBuffer/ChangeScreenBuffer) or an off-screen
 *                  bitmap + blit (RTG_F_BLIT); the real pixel format is read
 *                  from the bitmap
 *   rtg_present    path A (CLUT8 copy + LoadRGB32), path B (16/32-bit LUT
 *                  conversion with per-line runs), path C (dirty lines),
 *                  through a bitmap lock (P96 or CGX) or the OS copy calls
 *                  (RTG_F_OSCOPY)
 *   rtg_set_pacing / rtg_wait_frame   E-clock schedule (timer.device
 *                  UNIT_WAITECLOCK), switchable at any time
 *
 * Library bases are private (rtg_GfxBase ...), so the framework links into a
 * program that has its own GfxBase/IntuitionBase. SysBase comes from the C
 * runtime (libnix); the LoadSeg module form must read it from address 4
 * (README: "left for the module form").
 *
 * No calls are made while a bitmap is locked other than the converter
 * itself (no graphics, layers, Intuition, DOS or Wait). */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <devices/timer.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>
#include <graphics/displayinfo.h>
#include <graphics/modeid.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <cybergraphx/cybergraphics.h>
#include <utility/tagitem.h>

#define GRAPHICS_BASE_NAME      rtg_GfxBase
#define INTUITION_BASE_NAME     rtg_IntuitionBase
#define CYBERGRAPHICS_BASE_NAME rtg_CyberGfxBase
#define TIMER_BASE_NAME         rtg_TimerBase
#include <exec/libraries.h>
#include <exec/devices.h>
extern struct Library *rtg_GfxBase, *rtg_IntuitionBase, *rtg_CyberGfxBase;
extern struct Device  *rtg_TimerBase;
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>
#include <proto/timer.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "rtgfw.h"
#include "rtg_conv.h"
#include "../include/rtgfw_p96.h"

struct Library *rtg_GfxBase, *rtg_IntuitionBase, *rtg_CyberGfxBase, *P96Base_rtg;
struct Device  *rtg_TimerBase;

#define MAXMODES 2048          /* VideoCore lists 588 (26 Oct 2026); the first 128 are the small ones */
#define FLIP_TIMEOUT_US 100000u

typedef unsigned long long u64;

static struct {
	/* libraries */
	int libs_open;
	int p96ok;                  /* P96 calls passed the self-check */
	int lockm;                  /* 1 P96, 2 CGX */
	char info[200];
	/* timer */
	struct MsgPort *tport, *tport2;
	struct timerequest *treq, *treq2;   /* UNIT_WAITECLOCK (pacing), UNIT_MICROHZ (timeouts) */
	ULONG efreq;
	u64 period_fx, due_fx;              /* 48.16 E-clock ticks */
	/* screen */
	int open;
	struct Screen *scr;
	struct Window *win;
	UWORD *blankptr;
	struct ScreenBuffer *sb[RTGC_MAXBUF];
	struct MsgPort *safeport, *dispport;
	int nbuf;                   /* screen buffers in use (1 = single, direct to the screen bitmap) */
	int degraded;               /* messages were late: no more flips, keep writing the shown buffer */
	int blit;                   /* RTG_F_BLIT: off-screen bitmap */
	struct BitMap *offbm;
	int shown, last;            /* buffer on display; buffer written last */
	int prev_of[RTGC_MAXBUF];   /* buffer that was on display before this one */
	UBYTE safe_out[RTGC_MAXBUF], disp_out[RTGC_MAXBUF], writable[RTGC_MAXBUF];
	int oscopy;
	UBYTE *staging; int staging_bpr;
	/* layout and format */
	int scr_w, scr_h, ox, oy, sx, sy, pic_w, pic_h;
	int fmt, bpp, rawfmt;
	int src_w, src_h, vis_x, vis_y, vis_w, vis_h;
	int border_pen;
	ULONG surround_val[RTGC_MAXBUF];    /* value last painted around the picture, per buffer */
	UBYTE surround_need;                /* bit b: buffer b must repaint its surround */
	UWORD flags;
	RtgConv conv;
	UBYTE *written;
	ULONG pal32[2 + 256 * 3];
	void (*log)(const char *);
	RtgStats st;
	u64 last_disp;              /* E-clock of the last DispMessage seen while waiting */
	u64 refresh_sum; ULONG refresh_n;
	int errs;
} R;

static const char *const errtext[RTG_E_COUNT] = {
	"ok",
	"no RTG library (Picasso96API 2+ or cybergraphics 41+)",
	"no RTG mode with that depth and size",
	"out of memory",
	"could not open the RTG screen or window",
	"screen buffers refused",
	"pixel format not supported",
	"bad arguments",
	"RTG output not open",
	"palette runs refused (order, count, or an 8-bit screen)",
	"bitmap lock failed",
};

const char *rtg_error_text(RTG_REG(int32_t, err, d0))
{
	if (err < 0 || err >= RTG_E_COUNT) return "unknown error";
	return errtext[err];
}

static void logf_(int verbose, const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	if (!R.log || (verbose && !(R.flags & RTG_F_LOG))) return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	R.log(buf);
}

/* ------------------------------------------------------------------ */
/* time                                                                */

static int timer_open(void)
{
	if (R.treq) return 1;
	R.tport = CreateMsgPort();
	R.tport2 = CreateMsgPort();
	if (!R.tport || !R.tport2) goto fail;
	R.treq = (struct timerequest *)CreateIORequest(R.tport, sizeof(struct timerequest));
	R.treq2 = (struct timerequest *)CreateIORequest(R.tport2, sizeof(struct timerequest));
	if (!R.treq || !R.treq2) goto fail;
	if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_WAITECLOCK, (struct IORequest *)R.treq, 0)) { DeleteIORequest((struct IORequest *)R.treq); R.treq = NULL; goto fail; }
	if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, (struct IORequest *)R.treq2, 0)) { DeleteIORequest((struct IORequest *)R.treq2); R.treq2 = NULL; goto fail; }
	rtg_TimerBase = R.treq->tr_node.io_Device;
	{
		struct EClockVal ev;
		R.efreq = ReadEClock(&ev);
	}
	return 1;
fail:
	if (R.treq2) { if (R.treq2->tr_node.io_Device) CloseDevice((struct IORequest *)R.treq2); DeleteIORequest((struct IORequest *)R.treq2); R.treq2 = NULL; }
	if (R.treq) { CloseDevice((struct IORequest *)R.treq); DeleteIORequest((struct IORequest *)R.treq); R.treq = NULL; }
	if (R.tport) DeleteMsgPort(R.tport);
	if (R.tport2) DeleteMsgPort(R.tport2);
	R.tport = R.tport2 = NULL;
	rtg_TimerBase = NULL;
	return 0;
}

static void timer_close(void)
{
	if (R.treq2) { CloseDevice((struct IORequest *)R.treq2); DeleteIORequest((struct IORequest *)R.treq2); R.treq2 = NULL; }
	if (R.treq) { CloseDevice((struct IORequest *)R.treq); DeleteIORequest((struct IORequest *)R.treq); R.treq = NULL; }
	if (R.tport) DeleteMsgPort(R.tport);
	if (R.tport2) DeleteMsgPort(R.tport2);
	R.tport = R.tport2 = NULL;
	rtg_TimerBase = NULL;
	R.period_fx = R.due_fx = 0;
}

static u64 eclock(void)
{
	struct EClockVal ev;
	if (!rtg_TimerBase) return 0;
	ReadEClock(&ev);
	return ((u64)ev.ev_hi << 32) | ev.ev_lo;
}

static ULONG ticks_us(u64 t)
{
	if (!R.efreq) return 0;
	return (ULONG)((t / R.efreq) * 1000000ull + (t % R.efreq) * 1000000ull / R.efreq);
}

uint32_t rtg_x_micros(void) { if (!timer_open()) return 0; return ticks_us(eclock()); }
uint32_t rtg_x_eclock_hz(void) { return R.efreq; }

/* ------------------------------------------------------------------ */
/* libraries, modes                                                    */

static int map_rgbfb(ULONG f)
{
	switch (f) {
	case RGBFB_CLUT: return RTG_FMT_CLUT8;
	case RGBFB_R5G6B5: return RTG_FMT_RGB565;
	case RGBFB_R5G6B5PC: return RTG_FMT_RGB565PC;
	case RGBFB_A8R8G8B8: return RTG_FMT_ARGB32;
	case RGBFB_B8G8R8A8: return RTG_FMT_BGRA32;
	case RGBFB_R8G8B8A8: return RTG_FMT_RGBA32;
	}
	return RTG_FMT_NONE;
}

static int map_pixfmt(ULONG f)
{
	switch (f) {
	case PIXFMT_LUT8: return RTG_FMT_CLUT8;
	case PIXFMT_RGB16: return RTG_FMT_RGB565;
	case PIXFMT_RGB16PC: return RTG_FMT_RGB565PC;
	case PIXFMT_ARGB32: return RTG_FMT_ARGB32;
	case PIXFMT_BGRA32: return RTG_FMT_BGRA32;
	case PIXFMT_RGBA32: return RTG_FMT_RGBA32;
	}
	return RTG_FMT_NONE;
}

static const char *const fmtname[RTG_FMT_COUNT] = { "other", "CLUT8", "RGB565", "RGB565PC", "ARGB32", "BGRA32", "RGBA32" };

static ULONG libver(struct Library *l) { return l ? ((ULONG)l->lib_Version << 16) | l->lib_Revision : 0; }

static ULONG resident_lib_version(const char *name)
{
	struct Library *l;
	ULONG v = 0;
	Forbid();
	l = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)name);
	if (l) v = libver(l);
	Permit();
	return v;
}

static void libs_close(void)
{
	if (P96Base_rtg) CloseLibrary(P96Base_rtg);
	if (rtg_CyberGfxBase) CloseLibrary(rtg_CyberGfxBase);
	if (rtg_IntuitionBase) CloseLibrary(rtg_IntuitionBase);
	if (rtg_GfxBase) CloseLibrary(rtg_GfxBase);
	P96Base_rtg = rtg_CyberGfxBase = rtg_IntuitionBase = rtg_GfxBase = NULL;
	R.libs_open = 0;
	R.p96ok = 0;
}

static int dims_of(ULONG id, int *w, int *h, int *depth)
{
	struct DimensionInfo di;
	if (!GetDisplayInfoData(NULL, (UBYTE *)&di, sizeof di, DTAG_DIMS, id)) return 0;
	*w = di.Nominal.MaxX - di.Nominal.MinX + 1;
	*h = di.Nominal.MaxY - di.Nominal.MinY + 1;
	*depth = di.MaxDepth;
	return 1;
}

static int libs_open(void)
{
	ULONG id;
	int checked = 0, bad = 0;
	if (R.libs_open) return RTG_OK;
	rtg_GfxBase = OpenLibrary((CONST_STRPTR)"graphics.library", 40);
	rtg_IntuitionBase = OpenLibrary((CONST_STRPTR)"intuition.library", 39);
	if (!rtg_GfxBase || !rtg_IntuitionBase) { libs_close(); return RTG_E_NOLIB; }
	P96Base_rtg = OpenLibrary((CONST_STRPTR)"Picasso96API.library", 2);
	rtg_CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 41);
	if (!P96Base_rtg && !rtg_CyberGfxBase) { libs_close(); return RTG_E_NOLIB; }
	/* P96 self-check: p96GetModeIDAttr(width) must agree with graphics.library
	 * on every P96 mode, else the P96 calls are not trusted (see rtgfw_p96.h). */
	if (P96Base_rtg) {
		id = INVALID_ID;
		while ((id = NextDisplayInfo(id)) != INVALID_ID) {
			int w, h, d;
			if (!p96GetModeIDAttr(id, P96IDA_ISP96)) continue;
			if (!dims_of(id, &w, &h, &d)) continue;
			checked++;
			if ((int)p96GetModeIDAttr(id, P96IDA_WIDTH) != w || (int)p96GetModeIDAttr(id, P96IDA_HEIGHT) != h) bad++;
		}
		R.p96ok = checked > 0 && bad == 0;
	}
	R.lockm = R.p96ok ? 1 : 2;
	if (!R.p96ok && !rtg_CyberGfxBase) { libs_close(); return RTG_E_NOLIB; }
	snprintf(R.info, sizeof R.info, "Picasso96API %u.%u%s, rtg.library %u.%u, cybergraphics %u.%u, P96 self-check %d/%d modes %s",
	         (unsigned)(libver(P96Base_rtg) >> 16), (unsigned)(libver(P96Base_rtg) & 0xffff), P96Base_rtg ? "" : " (none)",
	         (unsigned)(resident_lib_version("rtg.library") >> 16), (unsigned)(resident_lib_version("rtg.library") & 0xffff),
	         (unsigned)(libver(rtg_CyberGfxBase) >> 16), (unsigned)(libver(rtg_CyberGfxBase) & 0xffff),
	         checked - bad, checked, R.p96ok ? "ok" : !P96Base_rtg ? "n/a" : checked ? "FAILED (P96 calls off)" : "(no P96 modes)");
	R.libs_open = 1;
	return RTG_OK;
}

const char *rtg_x_info(void) { return R.info; }

static int is_rtg(ULONG id)
{
	if (R.p96ok) return p96GetModeIDAttr(id, P96IDA_ISP96) != 0;
	if (rtg_CyberGfxBase) return IsCyberModeID(id) != 0;
	return 0;
}

static int mode_info(ULONG id, RtgModeInfo *m)
{
	int w, h, d;
	struct NameInfo ni;
	if (!is_rtg(id) || ModeNotAvailable(id) || !dims_of(id, &w, &h, &d)) return 0;
	memset(m, 0, sizeof *m);
	m->id = id; m->w = (uint16_t)w; m->h = (uint16_t)h; m->depth = (uint8_t)d;
	if (R.p96ok) {
		ULONG f = p96GetModeIDAttr(id, P96IDA_RGBFORMAT);
		m->rawfmt = (uint8_t)f; m->fmt = (uint8_t)map_rgbfb(f);
		m->depth = (uint8_t)p96GetModeIDAttr(id, P96IDA_BITSPERPIXEL);
	} else {
		ULONG f = GetCyberIDAttr(CYBRIDATTR_PIXFMT, id);
		m->rawfmt = (uint8_t)(100 + f); m->fmt = (uint8_t)map_pixfmt(f);
		m->depth = (uint8_t)GetCyberIDAttr(CYBRIDATTR_DEPTH, id);
	}
	if (GetDisplayInfoData(NULL, (UBYTE *)&ni, sizeof ni, DTAG_NAME, id)) {
		strncpy(m->name, (const char *)ni.Name, sizeof m->name - 1);
	}
	return 1;
}

int rtg_x_modes(RtgModeInfo *m, int max)
{
	ULONG id = INVALID_ID;
	int n = 0, opened = 0;
	if (!R.libs_open) { if (libs_open() != RTG_OK) return 0; opened = 1; }
	while (n < max && (id = NextDisplayInfo(id)) != INVALID_ID)
		if (mode_info(id, &m[n])) n++;
	if (opened && !R.open) libs_close();
	return n;
}

int32_t rtg_query(RTG_REG(RtgCaps *, caps, a0))
{
	RtgModeInfo mi;
	ULONG id = INVALID_ID;
	int err, opened = 0;
	if (!caps || caps->size < sizeof(RtgCaps)) return RTG_E_ARGS;
	caps->api = RTGFW_API;
	caps->system = 0; caps->depths = 0; caps->lib_version = 0; caps->board[0] = 0;
	if (!R.libs_open) {
		if ((err = libs_open()) != RTG_OK) return err;
		opened = 1;
	}
	caps->system = P96Base_rtg ? 1 : 2;
	caps->lib_version = (uint16_t)((P96Base_rtg ? P96Base_rtg : rtg_CyberGfxBase)->lib_Version);
	while ((id = NextDisplayInfo(id)) != INVALID_ID) {
		if (!mode_info(id, &mi)) continue;
		if (mi.fmt == RTG_FMT_CLUT8) caps->depths |= 1;
		else if (mi.fmt == RTG_FMT_RGB565 || mi.fmt == RTG_FMT_RGB565PC) caps->depths |= 2;
		else if (mi.fmt >= RTG_FMT_ARGB32) caps->depths |= 4;
		if (!caps->board[0] && R.p96ok) {
			const char *b = (const char *)p96GetModeIDAttr(id, P96IDA_BOARDNAME);
			if (b) { strncpy(caps->board, b, sizeof caps->board - 1); caps->board[sizeof caps->board - 1] = 0; }
		}
	}
	if (opened && !R.open) libs_close();
	return caps->depths ? RTG_OK : RTG_E_NOMODE;
}

/* ------------------------------------------------------------------ */
/* bitmap access                                                       */

static int fmt_of_bitmap(struct BitMap *bm, int *raw)
{
	if (R.p96ok) {
		ULONG f = p96GetBitMapAttr(bm, P96BMA_RGBFORMAT);
		*raw = (int)f;
		return map_rgbfb(f);
	}
	if (rtg_CyberGfxBase && GetCyberMapAttr(bm, CYBRMATTR_ISCYBERGFX)) {
		ULONG f = GetCyberMapAttr(bm, CYBRMATTR_PIXFMT);
		*raw = (int)(100 + f);
		return map_pixfmt(f);
	}
	*raw = -1;
	return RTG_FMT_NONE;
}

static APTR lock_bm(struct BitMap *bm, UBYTE **addr, LONG *bpr)
{
	if (R.lockm == 1) {
		struct RenderInfo ri;
		LONG l = p96LockBitMap(bm, (UBYTE *)&ri, sizeof ri);
		if (!l) return NULL;
		*addr = (UBYTE *)ri.Memory; *bpr = ri.BytesPerRow;
		return (APTR)l;
	} else {
		ULONG a = 0, b = 0;
		APTR h = LockBitMapTags(bm, LBMI_BASEADDRESS, (ULONG)&a, LBMI_BYTESPERROW, (ULONG)&b, TAG_DONE);
		if (!h) return NULL;
		*addr = (UBYTE *)a; *bpr = (LONG)b;
		return h;
	}
}

static void unlock_bm(struct BitMap *bm, APTR h)
{
	if (R.lockm == 1) p96UnlockBitMap(bm, (LONG)h);
	else UnLockBitMap(h);
}

int rtg_x_set_lock_method(int m)
{
	if (m == 1 && R.p96ok) R.lockm = 1;
	else if (m == 2 && rtg_CyberGfxBase) R.lockm = 2;
	else if (m == 0) R.lockm = R.p96ok ? 1 : 2;
	return R.lockm;
}

/* ------------------------------------------------------------------ */
/* screen buffer messages                                              */

static void note_disp(void)
{
	u64 t = eclock();
	if (R.last_disp && t > R.last_disp) {
		ULONG d = ticks_us(t - R.last_disp);
		if (d > 2000 && d < 100000) { R.refresh_sum += d; R.refresh_n++; R.st.refresh_us = (ULONG)(R.refresh_sum / R.refresh_n); }
	}
	R.last_disp = t;
}

static void drain(int waiting)
{
	struct Message *m;
	int i;
	if (!R.safeport) return;
	while ((m = GetMsg(R.safeport)) != NULL)
		for (i = 0; i < R.nbuf; i++)
			if (R.sb[i] && m == &R.sb[i]->sb_DBufInfo->dbi_SafeMessage) {
				R.safe_out[i] = 0;
				if (R.prev_of[i] >= 0) R.writable[R.prev_of[i]] = 1;
			}
	while ((m = GetMsg(R.dispport)) != NULL)
		for (i = 0; i < R.nbuf; i++)
			if (R.sb[i] && m == &R.sb[i]->sb_DBufInfo->dbi_DispMessage) {
				R.disp_out[i] = 0;
				if (waiting) note_disp(); else R.last_disp = 0;
			}
}

/* wait until cond() holds or timeout_us passes; returns 1 if it holds */
static int wait_msgs(int (*cond)(int), int arg, ULONG timeout_us)
{
	ULONG sigs;
	int ok;
	drain(0);
	if (cond(arg)) return 1;
	if (!R.treq2) return 0;
	R.treq2->tr_node.io_Command = TR_ADDREQUEST;
	R.treq2->tr_time.tv_secs = timeout_us / 1000000;
	R.treq2->tr_time.tv_micro = timeout_us % 1000000;
	SendIO((struct IORequest *)R.treq2);
	for (;;) {
		sigs = Wait((1UL << R.safeport->mp_SigBit) | (1UL << R.dispport->mp_SigBit) | (1UL << R.tport2->mp_SigBit));
		(void)sigs;
		drain(1);
		if ((ok = cond(arg)) != 0) break;
		if (CheckIO((struct IORequest *)R.treq2)) break;
	}
	if (!CheckIO((struct IORequest *)R.treq2)) AbortIO((struct IORequest *)R.treq2);
	WaitIO((struct IORequest *)R.treq2);
	SetSignal(0, 1UL << R.tport2->mp_SigBit);
	return cond(arg);
}

static int c_target_ready(int b) { return R.writable[b] && !R.safe_out[b] && !R.disp_out[b]; }
static int c_all_back(int unused) { int i; (void)unused; for (i = 0; i < R.nbuf; i++) if (R.safe_out[i] || R.disp_out[i]) return 0; return 1; }
static int c_disp_back(int unused) { int i; (void)unused; for (i = 0; i < R.nbuf; i++) if (R.disp_out[i]) return 0; return 1; }

/* ------------------------------------------------------------------ */
/* flip task (RTG_F_FLIPTASK, 27 Oct 2026)                             */
/*
 * The Emu68 VideoCore driver waits for the 60 Hz flip inside
 * ChangeScreenBuffer (8-9 ms a call, BENCH-RESULTS.md). With this flag a
 * small task owns the screen buffer messages and makes every
 * ChangeScreenBuffer call, so rtg_present only writes a free buffer and
 * hands it over:
 *   - the buffer states (shown, writable, safe_out, disp_out) change only in
 *     the flip task, under Forbid; rtg_present reads them under Forbid;
 *   - one buffer may wait in F.req; a newer frame takes it back and writes it
 *     again (a steal: the flip task is behind, the older frame is never shown);
 *   - with no free buffer the frame is dropped (rtgc_skip keeps its lines owed);
 *   - CLUT8: every handed-over buffer carries the whole palette it was written
 *     with (F.snap); the flip task loads the entries that differ from what is
 *     loaded right after its flip returns, so a frame and its palette change
 *     together (as near as the driver allows).
 * The task makes no DOS calls and logs nothing. It ends at close, after
 * every message is back and the screen's own bitmap is shown again. */

static struct {
	struct Task *task, *owner;
	UBYTE *stack;
	BYTE ack_bit;               /* owner's signal: task started / ended */
	ULONG go_mask;              /* task's signal: a buffer is queued, or quit */
	volatile int req;           /* buffer waiting to be shown, -1 none */
	volatile int flipping;      /* buffer inside ChangeScreenBuffer, -1 none */
	volatile int quit, ok, outstanding;
	int rel_pri;
	RtgFlipStats st;
	ULONG cur[256];             /* CLUT8: the palette as written (owner) */
	ULONG snap[RTGC_MAXBUF][256];
	ULONG loaded[256];          /* the palette in the CLUT now (task) */
	ULONG pal32[2 + 256 * 3];   /* LoadRGB32 table (task) */
} F = { 0, 0, 0, -1, 0, -1, -1, 0, 0, 0, 1 };

#define FT_STACK 4096

static void ft_load_clut(int b)
{
	int i = 0, lo, hi, n = 0;
	const ULONG *s = F.snap[b];
	while (i < 256 && s[i] == F.loaded[i]) i++;
	if (i == 256) return;
	lo = i;
	for (hi = 255; s[hi] == F.loaded[hi]; hi--) ;
	F.pal32[n++] = ((ULONG)(hi - lo + 1) << 16) | (ULONG)lo;
	for (i = lo; i <= hi; i++) {
		ULONG c = s[i];
		F.loaded[i] = c;
		F.pal32[n++] = ((c >> 16) & 0xff) * 0x01010101u;
		F.pal32[n++] = ((c >> 8) & 0xff) * 0x01010101u;
		F.pal32[n++] = (c & 0xff) * 0x01010101u;
	}
	F.pal32[n] = 0;
	LoadRGB32(&R.scr->ViewPort, F.pal32);
}

/* drain the reply ports (flip task only); states change under Forbid */
static void ft_drain(void)
{
	Forbid();
	drain(1);
	Permit();
}

static void ft_entry(void)
{
	struct MsgPort *sp = CreateMsgPort(), *dp = CreateMsgPort();
	BYTE go = AllocSignal(-1);
	int i, b, n;
	if (!sp || !dp || go < 0) {
		if (sp) DeleteMsgPort(sp);
		if (dp) DeleteMsgPort(dp);
		if (go >= 0) FreeSignal(go);
		F.ok = 0;
		Forbid();
		Signal(F.owner, 1UL << F.ack_bit);
		return;                              /* the task ends here, still under Forbid */
	}
	for (i = 0; i < R.nbuf; i++) {
		R.sb[i]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = sp;
		R.sb[i]->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = dp;
	}
	R.safeport = sp; R.dispport = dp;
	F.go_mask = 1UL << go;
	F.ok = 1;
	Signal(F.owner, 1UL << F.ack_bit);

	while (!F.quit) {
		Wait(F.go_mask | (1UL << sp->mp_SigBit) | (1UL << dp->mp_SigBit));
		ft_drain();
		for (;;) {
			u64 t0, t1;
			ULONG us;
			int ok;
			Forbid();
			b = F.req;
			if (b >= 0 && !F.quit) { F.req = -1; F.flipping = b; } else b = -1;
			Permit();
			if (b < 0) break;
			t0 = eclock();
			ok = ChangeScreenBuffer(R.scr, R.sb[b]);
			t1 = eclock();
			us = ticks_us(t1 - t0);
			F.st.us_flip += us;
			if (us > F.st.us_flip_max) F.st.us_flip_max = us;
			if (ok && R.fmt == RTG_FMT_CLUT8) { ft_load_clut(b); F.st.us_clut += ticks_us(eclock() - t1); }
			Forbid();
			if (ok) {
				R.safe_out[b] = R.disp_out[b] = 1;
				R.prev_of[b] = R.shown;
				R.writable[R.shown] = 0;
				R.writable[b] = 0;
				R.shown = b;
				F.st.flips++;
			} else F.st.fails++;             /* b stays writable: written but never shown */
			F.flipping = -1;
			Permit();
			ft_drain();
		}
	}

	/* close: every message back, then the screen's own bitmap on display */
	for (n = 0; n < 50 && !c_all_back(0); n++) { WaitTOF(); ft_drain(); }
	if (R.shown != 0 && c_all_back(0)) {
		for (n = 0; n < 10; n++) {
			if (ChangeScreenBuffer(R.scr, R.sb[0])) {
				Forbid(); R.safe_out[0] = R.disp_out[0] = 1; R.prev_of[0] = R.shown; R.shown = 0; Permit();
				break;
			}
			WaitTOF();
		}
		for (n = 0; n < 50 && !c_all_back(0); n++) { WaitTOF(); ft_drain(); }
	}
	F.outstanding = !c_all_back(0);
	if (!F.outstanding) { DeleteMsgPort(sp); DeleteMsgPort(dp); }
	R.safeport = R.dispport = NULL;          /* kept (leaked) if a message is still out */
	FreeSignal(go);
	Forbid();
	Signal(F.owner, 1UL << F.ack_bit);
	/* returning ends the task; Forbid holds until it is gone, so the owner
	 * cannot free the stack under it */
}

static int ft_start(void)
{
	struct Task *t;
	int pri;
	F.owner = FindTask(NULL);
	F.req = F.flipping = -1;
	F.quit = 0; F.ok = 0; F.outstanding = 0;
	memset(&F.st, 0, sizeof F.st);
	memset(F.loaded, 0xff, sizeof F.loaded);   /* never a colour: the first flip loads all in use */
	memset(F.cur, 0, sizeof F.cur);
	if ((F.ack_bit = AllocSignal(-1)) < 0) return 0;
	t = (struct Task *)AllocMem(sizeof(struct Task), MEMF_PUBLIC | MEMF_CLEAR);
	F.stack = (UBYTE *)AllocMem(FT_STACK, MEMF_PUBLIC | MEMF_CLEAR);
	if (!t || !F.stack) {
		if (t) FreeMem(t, sizeof(struct Task));
		if (F.stack) FreeMem(F.stack, FT_STACK);
		F.stack = NULL;
		FreeSignal(F.ack_bit); F.ack_bit = -1;
		return 0;
	}
	pri = F.owner->tc_Node.ln_Pri + F.rel_pri;
	if (pri > 20) pri = 20;
	if (pri < -20) pri = -20;
	t->tc_Node.ln_Type = NT_TASK;
	t->tc_Node.ln_Pri = (BYTE)pri;
	t->tc_Node.ln_Name = (char *)"rtgfw flip";
	t->tc_SPLower = F.stack;
	t->tc_SPUpper = F.stack + FT_STACK;
	t->tc_SPReg = F.stack + FT_STACK;
	/* the task makes its own reply ports (their signals must be its own); no
	 * flip has been made yet, so nothing can be in flight on these */
	if (R.safeport) DeleteMsgPort(R.safeport);
	if (R.dispport) DeleteMsgPort(R.dispport);
	R.safeport = R.dispport = NULL;
	SetSignal(0, 1UL << F.ack_bit);
	F.task = t;
	AddTask(t, (APTR)ft_entry, NULL);
	Wait(1UL << F.ack_bit);
	if (!F.ok) {                             /* the task has ended */
		FreeMem(t, sizeof(struct Task)); FreeMem(F.stack, FT_STACK);
		F.task = NULL; F.stack = NULL;
		FreeSignal(F.ack_bit); F.ack_bit = -1;
		return 0;
	}
	F.st.pri = pri;
	return 1;
}

static void ft_stop(void)
{
	if (!F.task) return;
	F.quit = 1;
	Signal(F.task, F.go_mask);
	Wait(1UL << F.ack_bit);                  /* the task has shown sb[0] and ended */
	FreeMem(F.task, sizeof(struct Task)); FreeMem(F.stack, FT_STACK);
	F.task = NULL; F.stack = NULL;
	FreeSignal(F.ack_bit); F.ack_bit = -1;
	if (F.outstanding) logf_(0, "rtgfw: flip task: screen buffer messages still out at close; ports kept");
	logf_(1, "rtgfw: flip task: %lu queued, %lu flips (%lu refused), %lu replaced, %lu dropped; flip avg %lu us, worst %lu us",
	      (unsigned long)F.st.queued, (unsigned long)F.st.flips, (unsigned long)F.st.fails, (unsigned long)F.st.steals,
	      (unsigned long)F.st.drops, (unsigned long)(F.st.flips ? F.st.us_flip / F.st.flips : 0), (unsigned long)F.st.us_flip_max);
}

void rtg_x_flip_stats(RtgFlipStats *s) { if (s) { *s = F.st; if (!F.task) s->pri = 0; } }
void rtg_x_set_flip_pri(int rel) { F.rel_pri = rel; }

/* vblank-timed writes (RTG_F_VBLANK): state here, code before present_common */
static struct {
	struct Task *task, *owner;
	UBYTE *stack;
	BYTE ack_bit;
	ULONG go_mask;
	volatile int quit, ok;
	int rel_pri;
	struct SignalSemaphore sem;
	UBYTE *stage;               /* vis_w x vis_h, the frame as presented */
	UBYTE *pend;                /* per visible line: presented since the last write */
	int pending;
	ULONG pal[256];
	int pal_lo, pal_hi;         /* palette entries changed since the last write */
	int border;                 /* new border pen, -1 none */
	/* timing, task only (E-clock ticks) */
	u64 vb0, vb, last_anchor;   /* first anchor, latest anchor */
	u64 per_fx;                 /* period, ticks << 16 */
	ULONG nb;                   /* blanks from vb0 to vb */
	ULONG delta;                /* ticks after the blank start to write */
	struct MsgPort *tp;
	struct timerequest *tr;
	char calib[100];            /* the calibration steps in us, for the log */
	RtgVblStats st;
} V = { 0, 0, 0, -1, 0, 0, 0, 1 };
static int vt_start(void);
static void vt_stop(void);
static int32_t vt_set_palette(int first, int count, const void *colours, int colfmt);

int32_t rtg_x_sync_display(uint32_t timeout_us, uint32_t *waited_us)
{
	u64 t0 = eclock();
	int ok;
	if (!R.open) return RTG_E_NOTOPEN;
	ok = R.nbuf > 1 && !F.task ? wait_msgs(c_disp_back, 0, timeout_us) : 1;
	if (waited_us) *waited_us = ticks_us(eclock() - t0);
	return ok ? RTG_OK : RTG_E_BUFFERS;
}

/* ------------------------------------------------------------------ */
/* open / close                                                        */

static void free_buffers(void)
{
	int i, outstanding = 0;
	if (F.task) ft_stop();                    /* it waits for the messages and shows sb[0] itself */
	else if (R.nbuf > 1 && R.scr) {
		logf_(1, "rtgfw: close: waiting for buffer messages (shown %d)", R.shown);
		wait_msgs(c_all_back, 0, 500000);
		if (R.shown != 0 && R.sb[0]) {
			/* show the screen's own bitmap again before freeing the others */
			int tries;
			for (tries = 0; tries < 10; tries++) {
				if (ChangeScreenBuffer(R.scr, R.sb[0])) { R.safe_out[0] = R.disp_out[0] = 1; R.prev_of[0] = R.shown; R.shown = 0; break; }
				WaitTOF();
			}
			wait_msgs(c_all_back, 0, 500000);
		}
		for (i = 0; i < R.nbuf; i++) if (R.safe_out[i] || R.disp_out[i]) outstanding = 1;
		logf_(1, "rtgfw: close: shown %d, messages outstanding %d", R.shown, outstanding);
	}
	for (i = 0; i < RTGC_MAXBUF; i++) {
		if (R.sb[i] && R.scr) FreeScreenBuffer(R.scr, R.sb[i]);
		R.sb[i] = NULL;
	}
	if (F.outstanding) { F.outstanding = 0; outstanding = 0; }   /* ports already kept by the flip task */
	else if (outstanding) {
		/* A message never came back: keep the ports (a small leak) rather
		 * than let the driver reply into freed memory later. */
		logf_(0, "rtgfw: screen buffer messages still out at close; ports kept");
		R.safeport = R.dispport = NULL;
	} else {
		if (R.safeport) DeleteMsgPort(R.safeport);
		if (R.dispport) DeleteMsgPort(R.dispport);
		R.safeport = R.dispport = NULL;
	}
	R.nbuf = 0;
	R.degraded = 0;
}

static void close_all(void)
{
	vt_stop();
	free_buffers();
	if (R.offbm) { WaitBlit(); FreeBitMap(R.offbm); R.offbm = NULL; }
	if (R.win) { ClearPointer(R.win); CloseWindow(R.win); R.win = NULL; }
	if (R.blankptr) { FreeMem(R.blankptr, 16); R.blankptr = NULL; }
	if (R.scr) { CloseScreen(R.scr); R.scr = NULL; }
	if (R.staging) { free(R.staging); R.staging = NULL; }
	if (R.written) { free(R.written); R.written = NULL; }
	rtgc_free(&R.conv);
	R.open = 0;
}

void rtg_close(void)
{
	if (R.open) logf_(1, "rtgfw: close");
	if (R.open || R.scr) close_all();
	timer_close();
	if (R.libs_open) libs_close();
}

static int fmt_group(int f)
{
	if (f == RTG_FMT_CLUT8) return 8;
	if (f == RTG_FMT_RGB565 || f == RTG_FMT_RGB565PC) return 16;
	if (f >= RTG_FMT_ARGB32 && f <= RTG_FMT_RGBA32) return 32;
	return 0;
}

static ULONG surround_value(int b)
{
	(void)b;
	if (R.fmt == RTG_FMT_CLUT8) return (ULONG)R.border_pen;
	return rtgc_pack(R.fmt, R.conv.base[R.border_pen]);
}

/* Try one mode: open the screen and window, check the real format. */
static int try_mode(RtgOpen *o, const RtgModeInfo *m, int buffers)
{
	int w, h, depth, raw, f;
	ULONG idcmp = IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS;
	if (!dims_of(m->id, &w, &h, &depth)) return RTG_E_NOMODE;
	R.scr = OpenScreenTags(NULL,
		SA_DisplayID, m->id, SA_Width, w, SA_Height, (o->flags & RTG_F_VBLANK) ? 2 * h : h, SA_Depth, depth,
		SA_Quiet, TRUE, SA_ShowTitle, FALSE, SA_Type, CUSTOMSCREEN,
		SA_Title, (ULONG)"rtgfw", SA_Draggable, FALSE, SA_Exclusive, TRUE,
		SA_AutoScroll, FALSE, SA_Interleaved, FALSE, TAG_DONE);
	if (!R.scr) { logf_(1, "rtgfw: OpenScreen %08lx %dx%dx%d failed", m->id, w, h, depth); return RTG_E_SCREEN; }
	f = fmt_of_bitmap(R.scr->RastPort.BitMap, &raw);
	if (fmt_group(f) == 0 || (o->depth && fmt_group(f) != o->depth)) {
		logf_(1, "rtgfw: mode %08lx bitmap format %d (raw %d) not usable", m->id, f, raw);
		CloseScreen(R.scr); R.scr = NULL;
		return RTG_E_FORMAT;
	}
	R.fmt = f; R.rawfmt = raw; R.bpp = rtgc_bpp(f);
	R.scr_w = w; R.scr_h = h;
	R.win = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)R.scr, WA_Left, 0, WA_Top, 0,
		WA_Width, w, WA_Height, h, WA_Backdrop, TRUE, WA_Borderless, TRUE, WA_Activate, TRUE,
		WA_RMBTrap, TRUE, WA_SimpleRefresh, TRUE, WA_NoCareRefresh, TRUE, WA_IDCMP, idcmp, TAG_DONE);
	if (!R.win) { CloseScreen(R.scr); R.scr = NULL; return RTG_E_SCREEN; }
	R.blankptr = (UWORD *)AllocMem(16, MEMF_CHIP | MEMF_CLEAR);
	if (R.blankptr) SetPointer(R.win, R.blankptr, 1, 16, 0, 0);
	(void)buffers;
	return RTG_OK;
}

static int alloc_buffers(int want)
{
	int i;
	R.nbuf = 1;
	R.degraded = 0;
	R.shown = R.last = 0;
	for (i = 0; i < RTGC_MAXBUF; i++) { R.prev_of[i] = -1; R.safe_out[i] = R.disp_out[i] = 0; R.writable[i] = 1; }
	R.writable[0] = want > 1 ? 0 : 1;
	if (want <= 1) return RTG_OK;
	R.safeport = CreateMsgPort();
	R.dispport = CreateMsgPort();
	if (!R.safeport || !R.dispport) goto fail;
	for (i = 0; i < want; i++) {
		R.sb[i] = AllocScreenBuffer(R.scr, NULL, i == 0 ? SB_SCREEN_BITMAP : 0);
		if (!R.sb[i]) goto fail;
		R.sb[i]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = R.safeport;
		R.sb[i]->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = R.dispport;
		R.sb[i]->sb_DBufInfo->dbi_UserData1 = (APTR)(LONG)i;
	}
	R.nbuf = want;
	return RTG_OK;
fail:
	for (i = 0; i < RTGC_MAXBUF; i++) { if (R.sb[i]) FreeScreenBuffer(R.scr, R.sb[i]); R.sb[i] = NULL; }
	if (R.safeport) DeleteMsgPort(R.safeport);
	if (R.dispport) DeleteMsgPort(R.dispport);
	R.safeport = R.dispport = NULL;
	R.nbuf = 1;
	R.writable[0] = 1;
	return RTG_E_BUFFERS;
}

static int cmp_area(const void *a, const void *b)
{
	const RtgModeInfo *x = a, *y = b;
	long ax = (long)x->w * x->h, ay = (long)y->w * y->h;
	return ax < ay ? -1 : ax > ay ? 1 : 0;
}

int32_t rtg_open(RTG_REG(RtgOpen *, o, a0))
{
	RtgModeInfo *modes;
	static const int order_auto[3] = { 8, 16, 32 }, order_perline[2] = { 16, 32 };
	const int *order;
	int norder, nm = 0, i, g, err = RTG_E_NOMODE, scl, need_w, need_h, wb_w = 0, wb_h = 0, want_buf, convfmt;
	ULONG id = INVALID_ID;
	RtgLayout L;

	if (!o || o->size < sizeof(RtgOpen) || o->api != RTGFW_API) return RTG_E_ARGS;
	if (R.open) rtg_close();
	if (!o->src_w || !o->src_h || !o->vis_w || !o->vis_h || o->vis_x + o->vis_w > o->src_w || o->vis_y + o->vis_h > o->src_h) return RTG_E_ARGS;
	if (o->flags & RTG_F_WINDOW) return RTG_E_ARGS;                      /* not in v0 */
	if ((o->flags & RTG_F_PERLINE) && o->depth == 8) return RTG_E_ARGS;
	if (o->buffers > 3 || o->scale > 8) return RTG_E_ARGS;
	if ((o->flags & RTG_F_BLIT) && (o->flags & RTG_F_OSCOPY)) return RTG_E_ARGS;
	if ((o->flags & RTG_F_VBLANK) && (o->flags & (RTG_F_BLIT | RTG_F_OSCOPY | RTG_F_FLIPTASK | RTG_F_PERLINE))) return RTG_E_ARGS;
	if (o->scan > RTG_SCAN_BLEND || o->scan_pct > 100) return RTG_E_ARGS;
	if (o->scan == RTG_SCAN_BLEND && o->depth == 8) return RTG_E_ARGS;   /* blended lines need 16/32-bit */
	if (o->scan && o->scale && (o->scale & 1)) return RTG_E_ARGS;        /* scanlines: even scales only */
	R.log = o->log; R.flags = o->flags;
	if ((err = libs_open()) != RTG_OK) return err;
	if (!timer_open()) { libs_close(); return RTG_E_NOMEM; }
	memset(&R.st, 0, sizeof R.st);
	R.refresh_sum = 0; R.refresh_n = 0; R.last_disp = 0; R.errs = 0;
	logf_(1, "rtgfw: %s", R.info);

	/* candidates: every RTG mode (the list was cut at 128 before 26 Oct 2026, so the big
	 * VideoCore modes were never seen), smallest first */
	modes = malloc(MAXMODES * sizeof *modes);
	if (!modes) { timer_close(); libs_close(); return RTG_E_NOMEM; }
	while (nm < MAXMODES && (id = NextDisplayInfo(id)) != INVALID_ID)
		if (mode_info(id, &modes[nm]) && fmt_group(modes[nm].fmt)) nm++;
	qsort(modes, nm, sizeof modes[0], cmp_area);
	logf_(1, "rtgfw: %d usable RTG modes", nm);
	if (o->depth) { static int one[1]; one[0] = o->depth; order = one; norder = 1; }
	else if ((o->flags & RTG_F_PERLINE) || o->scan == RTG_SCAN_BLEND) { order = order_perline; norder = 2; }
	else { order = order_auto; norder = 3; }
	scl = o->scale ? o->scale : (o->scan ? 2 : 1);
	need_w = o->vis_w * (o->aspect_x ? o->aspect_x : 1) * scl;
	need_h = o->vis_h * (o->aspect_y ? o->aspect_y : 1) * scl;
	if ((o->flags & RTG_F_NOHWSCALE) && !o->scale) {
		struct Screen *wb = LockPubScreen(NULL);
		if (wb) { wb_w = wb->Width; wb_h = wb->Height; UnlockPubScreen(NULL, wb); }
	}
	want_buf = (o->flags & (RTG_F_BLIT | RTG_F_VBLANK)) ? 1 : (o->buffers ? o->buffers : 3);

	err = RTG_E_NOMODE;
	if (o->mode_id) {
		RtgModeInfo mi;
		if (mode_info(o->mode_id, &mi)) err = try_mode(o, &mi, want_buf);
	}
	for (g = 0; !R.scr && g < norder; g++) {
		int pass;
		/* NOHWSCALE: first the Workbench size, then the smallest that fits */
		for (pass = (wb_w ? 0 : 1); !R.scr && pass < 2; pass++)
			for (i = 0; !R.scr && i < nm; i++) {
				if (fmt_group(modes[i].fmt) != order[g]) continue;
				if (modes[i].w < need_w || modes[i].h < need_h) continue;
				if (pass == 0 && (modes[i].w != wb_w || modes[i].h != wb_h)) continue;
				err = try_mode(o, &modes[i], want_buf);
			}
	}
	free(modes);
	if (!R.scr) { logf_(0, "rtgfw: open failed: %s", rtg_error_text(err)); rtg_close(); return err == RTG_OK ? RTG_E_NOMODE : err; }

	/* layout */
	if (o->scale) {
		L.sx = o->scale * (o->aspect_x ? o->aspect_x : 1); L.sy = o->scale * (o->aspect_y ? o->aspect_y : 1);
		L.w = o->vis_w * L.sx; L.h = o->vis_h * L.sy;
		L.x = (R.scr_w - L.w) / 2; L.y = (R.scr_h - L.h) / 2;
	} else if (o->scan) rtgc_layout_scan(o->vis_w, o->vis_h, o->aspect_x, o->aspect_y, R.scr_w, R.scr_h, &L);
	else rtgc_layout(o->vis_w, o->vis_h, o->aspect_x, o->aspect_y, R.scr_w, R.scr_h, &L);
	if (L.x < 0 || L.y < 0) { rtg_close(); return RTG_E_NOMODE; }
	R.ox = L.x; R.oy = L.y; R.sx = L.sx; R.sy = L.sy; R.pic_w = L.w; R.pic_h = L.h;
	R.src_w = o->src_w; R.src_h = o->src_h; R.vis_x = o->vis_x; R.vis_y = o->vis_y; R.vis_w = o->vis_w; R.vis_h = o->vis_h;
	R.border_pen = o->border_pen;
	R.oscopy = (o->flags & RTG_F_OSCOPY) != 0;
	R.blit = (o->flags & RTG_F_BLIT) != 0;

	/* buffers */
	if (R.blit) {
		R.offbm = AllocBitMap(R.scr_w, R.scr_h, R.scr->RastPort.BitMap->Depth, BMF_MINPLANES | BMF_DISPLAYABLE, R.scr->RastPort.BitMap);
		if (!R.offbm) { rtg_close(); return RTG_E_NOMEM; }
		alloc_buffers(1);
		{
			int raw2, f2 = fmt_of_bitmap(R.offbm, &raw2);
			if (f2 != R.fmt) { logf_(0, "rtgfw: off-screen bitmap format %d differs from the screen's %d", f2, R.fmt); rtg_close(); return RTG_E_FORMAT; }
		}
	} else if (alloc_buffers((o->flags & RTG_F_FLIPTASK) ? 3 : want_buf) != RTG_OK) {
		logf_(0, "rtgfw: AllocScreenBuffer refused, single buffer");
	} else if ((o->flags & RTG_F_FLIPTASK) && !ft_start()) {
		logf_(0, "rtgfw: flip task did not start, single buffer");
		free_buffers();
		alloc_buffers(1);
	}

	/* converter: OS copy of direct formats without P96 goes through ARGB32 + WritePixelArray(RECTFMT_ARGB) */
	convfmt = R.fmt;
	if (R.oscopy && R.fmt != RTG_FMT_CLUT8 && !R.p96ok) convfmt = RTG_FMT_ARGB32;
	if ((err = rtgc_init(&R.conv, R.vis_w, R.vis_h, convfmt, R.nbuf)) != RTG_OK) { rtg_close(); return err; }
	R.conv.sx = R.sx; R.conv.sy = R.sy; R.conv.border = R.border_pen;
	if (o->scan && (err = rtgc_set_scan(&R.conv, o->scan, o->scan_pct)) != RTG_OK) { rtg_close(); return err; }
	R.written = malloc(R.vis_h);
	if (!R.written) { rtg_close(); return RTG_E_NOMEM; }
	R.conv.written = R.written;
	if (R.oscopy || R.blit) {
		/* OS copy: a fast-RAM staging frame. Blit: lines go to the off-screen bitmap. */
		if (R.oscopy) {
			R.staging_bpr = R.pic_w * rtgc_bpp(convfmt);
			R.staging = malloc((long)R.staging_bpr * R.pic_h);
			if (!R.staging) { rtg_close(); return RTG_E_NOMEM; }
		}
	}
	R.surround_need = (UBYTE)((1 << RTGC_MAXBUF) - 1);
	R.open = 1;
	if (o->flags & RTG_F_VBLANK) vt_start();   /* else (or if it fails) plain direct writes */

	o->out_mode_id = GetVPModeID(&R.scr->ViewPort);
	o->out_w = (uint16_t)R.scr_w; o->out_h = (uint16_t)R.scr_h;
	o->out_x = (uint16_t)R.ox; o->out_y = (uint16_t)R.oy;
	o->out_scale_x = (uint16_t)R.sx; o->out_scale_y = (uint16_t)R.sy;
	o->out_fmt = (uint8_t)R.fmt;
	o->out_buffers = (uint8_t)(R.blit ? 0 : R.nbuf);
	o->out_screen = R.scr; o->out_window = R.win;
	logf_(1, "rtgfw: open %08lx %dx%d %s, picture %dx%d at %d,%d (x%d,%d), %d buffer(s)%s%s, lock %s, scanlines %s %d%%",
	      o->out_mode_id, R.scr_w, R.scr_h, fmtname[R.fmt], R.pic_w, R.pic_h, R.ox, R.oy, R.sx, R.sy, R.nbuf,
	      R.blit ? " blit" : "", R.oscopy ? " oscopy" : "", R.lockm == 1 ? "P96" : "CGX",
	      o->scan == RTG_SCAN_HARD ? "hard" : o->scan == RTG_SCAN_BLEND ? "blend" : "off", (int)o->scan_pct);
	return RTG_OK;
}

/* ------------------------------------------------------------------ */
/* palette, border, pacing                                             */

int32_t rtg_set_palette(RTG_REG(int32_t, line, d0), RTG_REG(int32_t, first, d1), RTG_REG(int32_t, count, d2),
                        RTG_REG(const void *, colours, a0), RTG_REG(int32_t, colfmt, d3))
{
	if (!R.open) return RTG_E_NOTOPEN;
	if (!colours || (colfmt != RTG_RGB24 && colfmt != RTG_RGB12)) return RTG_E_ARGS;
	if (line <= 0) return V.task ? vt_set_palette(first, count, colours, colfmt) : rtgc_set_palette(&R.conv, 0, first, count, colours, colfmt);
	if (V.task) return RTG_E_RUNS;
	line -= R.vis_y;
	if (line >= R.vis_h) return RTG_OK;                     /* below the visible part */
	return rtgc_add_run(&R.conv, line < 0 ? 0 : line, first, count, colours, colfmt);
}

int32_t rtg_set_border(RTG_REG(int32_t, pen, d0))
{
	if (!R.open) return RTG_E_NOTOPEN;
	if (pen < 0 || pen > 255) return RTG_E_ARGS;
	if (V.task) { ObtainSemaphore(&V.sem); V.border = pen; V.pending = 1; ReleaseSemaphore(&V.sem); return RTG_OK; }
	if (pen != R.border_pen) {
		R.border_pen = pen; R.conv.border = pen;
		R.surround_need = (UBYTE)((1 << RTGC_MAXBUF) - 1);
		rtgc_invalidate(&R.conv);                            /* RTG_L_BLANK lines use the pen */
	}
	return RTG_OK;
}

void rtg_set_pacing(RTG_REG(uint32_t, frame_us, d0))
{
	if (!frame_us) { R.period_fx = 0; R.due_fx = 0; return; }
	if (!timer_open()) return;
	R.period_fx = ((u64)frame_us * R.efreq << 16) / 1000000ull;
	/* the next due time follows from the last one: no jump on a 50/60 Hz switch */
}

int32_t rtg_wait_frame(void)
{
	u64 now;
	if (!R.period_fx || !R.treq) return 0;
	now = eclock() << 16;
	if (!R.due_fx) R.due_fx = now;
	R.due_fx += R.period_fx;
	if (now >= R.due_fx) {
		u64 behind = now - R.due_fx;
		if (behind > 4 * R.period_fx) R.due_fx = now;       /* far behind: restart the schedule */
		return (int32_t)ticks_us(behind >> 16);
	}
	{
		u64 due = R.due_fx >> 16;
		R.treq->tr_node.io_Command = TR_ADDREQUEST;
		R.treq->tr_time.tv_secs = (ULONG)(due >> 32);       /* UNIT_WAITECLOCK: an EClockVal */
		R.treq->tr_time.tv_micro = (ULONG)due;
		DoIO((struct IORequest *)R.treq);
	}
	return 0;
}

void rtg_get_stats(RTG_REG(RtgStats *, s, a0))
{
	if (s) *s = R.st;
}

/* ------------------------------------------------------------------ */
/* present                                                             */

static void fill_rect(UBYTE *base, LONG bpr, int x, int y, int w, int h, ULONG v)
{
	int r, i;
	if (w <= 0 || h <= 0) return;
	for (r = 0; r < h; r++) {
		UBYTE *d = base + (long)(y + r) * bpr + (long)x * R.bpp;
		if (R.bpp == 1) memset(d, (int)v, w);
		else if (R.bpp == 2) { UWORD *p = (UWORD *)d; for (i = w; i; i--) *p++ = (UWORD)v; }
		else { ULONG *p = (ULONG *)d; for (i = w; i; i--) *p++ = v; }
	}
}

static void fill_surround(UBYTE *base, LONG bpr, ULONG v)
{
	fill_rect(base, bpr, 0, 0, R.scr_w, R.oy, v);
	fill_rect(base, bpr, 0, R.oy + R.pic_h, R.scr_w, R.scr_h - R.oy - R.pic_h, v);
	fill_rect(base, bpr, 0, R.oy, R.ox, R.pic_h, v);
	fill_rect(base, bpr, R.ox + R.pic_w, R.oy, R.scr_w - R.ox - R.pic_w, R.pic_h, v);
}

static void load_clut(int lo, int hi)
{
	int i, n = 0;
	if (lo > hi) return;
	R.pal32[n++] = ((ULONG)(hi - lo + 1) << 16) | (ULONG)lo;
	for (i = lo; i <= hi; i++) {
		ULONG c = rtgc_clut_colour(&R.conv, i);    /* the base colour, or its dimmed copy (scanlines) */
		R.pal32[n++] = ((c >> 16) & 0xff) * 0x01010101u;
		R.pal32[n++] = ((c >> 8) & 0xff) * 0x01010101u;
		R.pal32[n++] = (c & 0xff) * 0x01010101u;
	}
	R.pal32[n] = 0;
	LoadRGB32(&R.scr->ViewPort, R.pal32);
	R.st.palette_loads++;
}

/* OS copy of the lines just converted into the staging frame */
static void os_copy_spans(struct RastPort *rp)
{
	int y = 0, y1;
	while (y < R.vis_h) {
		if (!R.written[y]) { y++; continue; }
		y1 = y;
		while (y1 < R.vis_h && R.written[y1]) y1++;
		{
			int sy0 = y * R.sy, rows = (y1 - y) * R.sy;
			UBYTE *src = R.staging + (long)sy0 * R.staging_bpr;
			if (R.fmt == RTG_FMT_CLUT8)
				WriteChunkyPixels(rp, R.ox, R.oy + sy0, R.ox + R.pic_w - 1, R.oy + sy0 + rows - 1, src, R.staging_bpr);
			else if (R.p96ok) {
				struct RenderInfo ri;
				ri.Memory = R.staging; ri.BytesPerRow = (WORD)R.staging_bpr; ri.pad = 0; ri.RGBFormat = (ULONG)R.rawfmt;
				p96WritePixelArray(&ri, 0, (UWORD)sy0, rp, (UWORD)R.ox, (UWORD)(R.oy + sy0), (UWORD)R.pic_w, (UWORD)rows);
			} else
				WritePixelArray(R.staging, 0, sy0, R.staging_bpr, rp, R.ox, R.oy + sy0, R.pic_w, rows, RECTFMT_ARGB);
		}
		y = y1;
	}
}

static void blit_spans(void)
{
	int y = 0, y1;
	while (y < R.vis_h) {
		if (!R.written[y]) { y++; continue; }
		y1 = y;
		while (y1 < R.vis_h && R.written[y1]) y1++;
		BltBitMapRastPort(R.offbm, R.ox, R.oy + y * R.sy, &R.scr->RastPort, R.ox, R.oy + y * R.sy, R.pic_w, (y1 - y) * R.sy, 0xc0);
		y = y1;
	}
}

/* ------------------------------------------------------------------ */
/* vblank-timed writes (RTG_F_VBLANK, 27 Oct 2026)                     */
/*
 * One buffer, no flips, and still no tearing: the copy into board memory is
 * far faster than the display scans it out (a CPC frame 0.2-0.8 ms against a
 * 16.7 ms refresh), so a frame written just after the refresh's vertical
 * blank starts stays ahead of the scan all the way down.
 *
 * Finding the blank. The Emu68 VideoCore driver gives no vertical blank
 * signal, but a pan (ScrollVPort to another RyOffset) busy-waits until the
 * display is inside its blank and returns there; a second pan inside the
 * same blank returns at once (panbench, 27 Oct 2026). So the screen is
 * opened two pages tall: page 0 is the picture, page 1 is only ever panned
 * to and straight back within one blank. Seven such anchors at open give
 * the period; one anchor every second, 300 us before a predicted blank,
 * keeps the phase (prediction error on the A1200: under 10 us) and refines
 * the period over a growing baseline.
 *
 * Work split:
 *   rtg_present (caller): copies its dirty lines into a staging frame,
 *     marks them, signals; no lock, no wait (well under 0.1 ms).
 *   the vblank task (caller's priority + 1): at the next blank + 50 us it
 *     applies the palette and border changes, writes the marked lines
 *     through the converter, loads the CLUT range, clears the marks.
 *   A semaphore guards the staging frame, the marks and the pending palette;
 *   the caller waits for it only if it presents during a write.
 * Frames presented faster than the refresh merge (the newest lines win).
 * Palette runs and line flags are not supported in this mode (RTG_E_RUNS /
 * RTG_E_ARGS). If the anchors do not give a steady period (a board that
 * returns at once, e.g. WinUAE's uaegfx), rtg_open falls back to plain
 * direct writes and logs it. */


#define VT_STACK 8192

static void vt_wait_until(u64 t)
{
	V.tr->tr_node.io_Command = TR_ADDREQUEST;
	V.tr->tr_time.tv_secs = (ULONG)(t >> 32);
	V.tr->tr_time.tv_micro = (ULONG)t;
	DoIO((struct IORequest *)V.tr);
}

static u64 us_ticks(ULONG us) { return ((u64)us * R.efreq + 500000) / 1000000; }

/* pan to page 1 and straight back: returns the E-clock at the blank start */
static u64 vt_pan_pair(void)
{
	struct ViewPort *vp = &R.scr->ViewPort;
	u64 t;
	vp->RasInfo->RyOffset = (WORD)R.scr_h;
	ScrollVPort(vp);
	t = eclock();
	vp->RasInfo->RyOffset = 0;
	ScrollVPort(vp);
	return t;
}

/* the next predicted blank start at or after t */
static u64 vt_next_blank(u64 t)
{
	u64 k;
	if (t <= V.vb) return V.vb;
	k = (((t - V.vb) << 16) + V.per_fx - 1) / V.per_fx;
	return V.vb + ((k * V.per_fx) >> 16);
}

/* 9 anchors, each 2 ms after the last blank (so the next pan waits for the next
 * one), after one to settle. The steps must all be whole multiples (1-3) of the
 * shortest, within 400 us: a pan may miss a blank while the board is busy (seen
 * on the first 8-bit screen, 27 Oct 2026). 1 if they agree. */
static int vt_calibrate(void)
{
	u64 a[9];
	ULONG d[8], dmin = ~0ul, k, K = 0;
	int i, n = 0;
	vt_pan_pair();
	vt_wait_until(eclock() + us_ticks(2000));
	for (i = 0; i < 9; i++) {
		a[i] = vt_pan_pair();
		if (i < 8) vt_wait_until(a[i] + us_ticks(2000));
	}
	for (i = 0; i < 8; i++) {
		d[i] = ticks_us(a[i + 1] - a[i]);
		if (d[i] < dmin) dmin = d[i];
		n += snprintf(V.calib + n, sizeof V.calib - n, " %lu", (unsigned long)d[i]);
		if (n >= (int)sizeof V.calib) n = sizeof V.calib - 1;
	}
	V.st.period_us = dmin;
	if (dmin < 8000 || dmin > 25000) return 0;
	for (i = 0; i < 8; i++) {
		k = (d[i] + dmin / 2) / dmin;
		if (k < 1 || k > 3) return 0;
		if (d[i] > k * dmin + 400 || d[i] + 400 < k * dmin) return 0;
		K += k;
	}
	V.per_fx = ((a[8] - a[0]) << 16) / K;
	V.st.period_us = (ULONG)((V.per_fx * 1000000ull / R.efreq) >> 16);
	V.vb0 = a[0]; V.vb = a[8]; V.nb = K; V.last_anchor = a[8];
	return 1;
}

static void vt_anchor(void)
{
	u64 p = vt_next_blank(eclock() + us_ticks(600)), r;
	int32_t e;
	vt_wait_until(p - us_ticks(300));
	r = vt_pan_pair();
	e = (int32_t)((int64_t)(r - p) * 1000000 / (int64_t)R.efreq);
	V.st.anchors++;
	if (e > -1500 && e < 1500) {
		ULONG n = (ULONG)((((r - V.vb0) << 16) + V.per_fx / 2) / V.per_fx);
		if (e < 0) e = -e;
		if ((ULONG)e > V.st.anchor_err_max) V.st.anchor_err_max = (ULONG)e;
		if (n > V.nb) { V.per_fx = ((r - V.vb0) << 16) / n; V.nb = n; }
		V.vb = r;
		V.st.period_us = (ULONG)((V.per_fx * 1000000ull / R.efreq) >> 16);
	} else V.st.anchor_fails++;
	V.last_anchor = eclock();
}

static void vt_write(void)
{
	struct BitMap *bm = R.scr->RastPort.BitMap;
	UBYTE *addr = NULL;
	LONG bpr = 0;
	APTR h;
	int lo, hi;
	u64 t0 = eclock();
	ULONG us, lw = R.conv.lines_written, bw = R.conv.bytes_written;
	ObtainSemaphore(&V.sem);
	if (V.pal_lo <= V.pal_hi) {
		rtgc_set_palette(&R.conv, 0, V.pal_lo, V.pal_hi - V.pal_lo + 1, &V.pal[V.pal_lo], RTG_RGB24);
		V.pal_lo = 256; V.pal_hi = -1;
	}
	if (V.border >= 0) {
		if (V.border != R.border_pen) {
			R.border_pen = V.border; R.conv.border = V.border;
			R.surround_need = 1;
			rtgc_invalidate(&R.conv);
		}
		V.border = -1;
	}
	h = lock_bm(bm, &addr, &bpr);
	if (!h || !addr) {
		if (h) unlock_bm(bm, h);
		R.errs++;
		ReleaseSemaphore(&V.sem);            /* marks kept: tried again at the next blank */
		return;
	}
	if (R.surround_need & 1) { fill_surround(addr, bpr, surround_value(0)); R.surround_need &= (UBYTE)~1; R.surround_val[0] = surround_value(0); }
	rtgc_present(&R.conv, 0, V.stage, R.vis_w, V.pend, NULL, addr + (long)R.oy * bpr + (long)R.ox * R.bpp, bpr, &lo, &hi);
	unlock_bm(bm, h);
	if (R.fmt == RTG_FMT_CLUT8) load_clut(lo, hi);
	memset(V.pend, 0, R.vis_h);
	V.pending = 0;
	ReleaseSemaphore(&V.sem);
	us = ticks_us(eclock() - t0);
	V.st.writes++;
	V.st.us_write += us;
	if (us > V.st.us_write_max) V.st.us_write_max = us;
	R.st.lines_written += R.conv.lines_written - lw;
	R.st.bytes_written += R.conv.bytes_written - bw;
	R.st.us_convert += us;
}

static void vt_entry(void)
{
	BYTE go = AllocSignal(-1);
	V.tp = CreateMsgPort();
	V.tr = V.tp ? (struct timerequest *)CreateIORequest(V.tp, sizeof(struct timerequest)) : NULL;
	if (go < 0 || !V.tr || OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_WAITECLOCK, (struct IORequest *)V.tr, 0)) {
		if (V.tr) { DeleteIORequest((struct IORequest *)V.tr); V.tr = NULL; }
		if (V.tp) { DeleteMsgPort(V.tp); V.tp = NULL; }
		if (go >= 0) FreeSignal(go);
		V.ok = 0;
		Forbid();
		Signal(V.owner, 1UL << V.ack_bit);
		return;
	}
	V.go_mask = 1UL << go;
	V.ok = vt_calibrate();
	if (V.ok) Signal(V.owner, 1UL << V.ack_bit);   /* if not, the one signal is the one at the end */
	while (V.ok && !V.quit) {
		u64 now, b;
		Wait(V.go_mask);
		while (V.pending && !V.quit) {
			now = eclock();
			b = vt_next_blank(now);           /* the first blank at or after now */
			if (b - now > (V.per_fx >> 16) - us_ticks(250)) V.st.at_once++;   /* the one before was under 250 us ago: still at the top, write now */
			else {
				ULONG late;
				vt_wait_until(b + V.delta);
				now = eclock();
				late = now > b + V.delta ? ticks_us(now - (b + V.delta)) : 0;
				if (late > V.st.late_max_us) V.st.late_max_us = late;
			}
			vt_write();
			if (eclock() - V.last_anchor > (u64)R.efreq) vt_anchor();
		}
	}
	CloseDevice((struct IORequest *)V.tr);
	DeleteIORequest((struct IORequest *)V.tr); V.tr = NULL;
	DeleteMsgPort(V.tp); V.tp = NULL;
	FreeSignal(go);
	Forbid();
	Signal(V.owner, 1UL << V.ack_bit);
}

static void vt_free(void)
{
	if (V.stage) { free(V.stage); V.stage = NULL; }
	if (V.pend) { free(V.pend); V.pend = NULL; }
}

/* after rtg_open has set up the screen (two pages) and the converter */
static int vt_start(void)
{
	struct Task *t;
	int pri;
	V.owner = FindTask(NULL);
	V.quit = 0; V.ok = 0; V.pending = 0;
	V.pal_lo = 256; V.pal_hi = -1; V.border = -1;
	V.delta = (ULONG)us_ticks(50);
	memset(&V.st, 0, sizeof V.st);
	V.calib[0] = 0;
	InitSemaphore(&V.sem);
	V.stage = malloc((long)R.vis_w * R.vis_h);
	V.pend = calloc(R.vis_h, 1);
	if (!V.stage || !V.pend) { vt_free(); return 0; }
	memset(V.stage, R.border_pen, (long)R.vis_w * R.vis_h);
	{
		/* page 1 in the border pen: it is never meant to be seen */
		UBYTE *addr; LONG bpr;
		APTR h = lock_bm(R.scr->RastPort.BitMap, &addr, &bpr);
		if (h) { fill_rect(addr + (long)R.scr_h * bpr, bpr, 0, 0, R.scr_w, R.scr_h, surround_value(0)); unlock_bm(R.scr->RastPort.BitMap, h); }
	}
	if ((V.ack_bit = AllocSignal(-1)) < 0) { vt_free(); return 0; }
	t = (struct Task *)AllocMem(sizeof(struct Task), MEMF_PUBLIC | MEMF_CLEAR);
	V.stack = (UBYTE *)AllocMem(VT_STACK, MEMF_PUBLIC | MEMF_CLEAR);
	if (!t || !V.stack) {
		if (t) FreeMem(t, sizeof(struct Task));
		if (V.stack) FreeMem(V.stack, VT_STACK);
		V.stack = NULL;
		FreeSignal(V.ack_bit); V.ack_bit = -1;
		vt_free();
		return 0;
	}
	pri = V.owner->tc_Node.ln_Pri + V.rel_pri;
	if (pri > 20) pri = 20;
	if (pri < -20) pri = -20;
	t->tc_Node.ln_Type = NT_TASK;
	t->tc_Node.ln_Pri = (BYTE)pri;
	t->tc_Node.ln_Name = (char *)"rtgfw vblank";
	t->tc_SPLower = V.stack;
	t->tc_SPUpper = V.stack + VT_STACK;
	t->tc_SPReg = V.stack + VT_STACK;
	SetSignal(0, 1UL << V.ack_bit);
	V.task = t;
	AddTask(t, (APTR)vt_entry, NULL);
	Wait(1UL << V.ack_bit);                  /* calibrated (about 0.1 s) or failed */
	if (!V.ok) {
		FreeMem(t, sizeof(struct Task)); FreeMem(V.stack, VT_STACK);
		V.task = NULL; V.stack = NULL; V.go_mask = 0;
		FreeSignal(V.ack_bit); V.ack_bit = -1;
		logf_(0, "rtgfw: vblank timing: no steady refresh from the pans (steps%s us), plain direct writes", V.calib);
		vt_free();
		return 0;
	}
	V.st.pri = pri;
	logf_(1, "rtgfw: vblank timing: refresh %lu us (steps%s), task pri %d", (unsigned long)V.st.period_us, V.calib, pri);
	return 1;
}

static void vt_stop(void)
{
	if (!V.task) return;
	V.quit = 1;
	Signal(V.task, V.go_mask);
	Wait(1UL << V.ack_bit);
	FreeMem(V.task, sizeof(struct Task)); FreeMem(V.stack, VT_STACK);
	V.task = NULL; V.stack = NULL; V.go_mask = 0;
	FreeSignal(V.ack_bit); V.ack_bit = -1;
	logf_(1, "rtgfw: vblank timing: %lu presents, %lu writes (%lu at once), %lu merged; write avg %lu max %lu us, late max %lu us; "
	      "%lu anchors (%lu off), error max %lu us, refresh %lu us",
	      (unsigned long)V.st.presents, (unsigned long)V.st.writes, (unsigned long)V.st.at_once, (unsigned long)V.st.merged,
	      (unsigned long)(V.st.writes ? V.st.us_write / V.st.writes : 0), (unsigned long)V.st.us_write_max, (unsigned long)V.st.late_max_us,
	      (unsigned long)V.st.anchors, (unsigned long)V.st.anchor_fails, (unsigned long)V.st.anchor_err_max, (unsigned long)V.st.period_us);
	vt_free();
}

/* rtg_present in this mode: the dirty lines into the staging frame */
static int32_t vt_present(const uint8_t *pix, int32_t stride, const uint8_t *dirty)
{
	int y;
	u64 t0 = eclock();
	ObtainSemaphore(&V.sem);
	for (y = 0; y < R.vis_h; y++, pix += stride)
		if (!dirty || dirty[y]) { memcpy(V.stage + (long)y * R.vis_w, pix, R.vis_w); V.pend[y] = 1; }
	if (V.pending) V.st.merged++;
	V.pending = 1;
	V.st.presents++;
	ReleaseSemaphore(&V.sem);
	Signal(V.task, V.go_mask);
	R.st.frames++;
	R.st.us_flip_wait += ticks_us(eclock() - t0);   /* the caller's whole cost here */
	return RTG_OK;
}

static int32_t vt_set_palette(int first, int count, const void *colours, int colfmt)
{
	int i;
	if (first < 0 || count < 0 || first + count > 256) return RTG_E_ARGS;
	ObtainSemaphore(&V.sem);
	for (i = 0; i < count; i++) {
		ULONG c;
		if (colfmt == RTG_RGB12) {
			UWORD w = ((const UWORD *)colours)[i];
			c = ((ULONG)((w >> 8) & 15) * 0x110000u) | ((ULONG)((w >> 4) & 15) * 0x1100u) | ((ULONG)(w & 15) * 0x11u);
		} else c = ((const ULONG *)colours)[i] & 0xffffff;
		V.pal[first + i] = c;
	}
	if (count) {
		if (first < V.pal_lo) V.pal_lo = first;
		if (first + count - 1 > V.pal_hi) V.pal_hi = first + count - 1;
		V.pending = 1;
	}
	ReleaseSemaphore(&V.sem);
	return RTG_OK;
}

/* wait until the last presented frame is on the screen (for grabs) */
int32_t rtg_x_vbl_sync(uint32_t timeout_us)
{
	ULONG waited = 0;
	if (!V.task) return RTG_OK;
	while (V.pending && waited < timeout_us && R.treq2) {
		R.treq2->tr_node.io_Command = TR_ADDREQUEST;
		R.treq2->tr_time.tv_secs = 0;
		R.treq2->tr_time.tv_micro = 1000;
		DoIO((struct IORequest *)R.treq2);
		waited += 1000;
	}
	return V.pending ? RTG_E_BUFFERS : RTG_OK;
}

void rtg_x_vbl_stats(RtgVblStats *s) { if (s) { *s = V.st; if (!V.task) s->pri = 0; } }
void rtg_x_set_vbl_pri(int rel) { V.rel_pri = rel; }

static int32_t present_common(const uint8_t *pixels, int32_t stride, const uint8_t *dirty, const uint8_t *lineflags)
{
	struct BitMap *bm;
	int b = 0, lo, hi, err = RTG_OK;
	u64 t0, t1, t2, t3;
	const uint8_t *pix;
	if (!R.open) return RTG_E_NOTOPEN;
	if (!pixels || stride < R.vis_w) return RTG_E_ARGS;
	pix = pixels + (long)R.vis_y * stride + R.vis_x;
	if (dirty) dirty += R.vis_y;
	if (lineflags) lineflags += R.vis_y;
	if (V.task) {
		if (lineflags || R.conv.nruns) { R.conv.nruns = 0; R.conv.pool_used = 0; return lineflags ? RTG_E_ARGS : RTG_E_RUNS; }
		return vt_present(pix, stride, dirty);
	}

	/* pick the buffer */
	t0 = eclock();
	if (F.task) {
		int i;
		Forbid();
		if (F.req >= 0) { b = F.req; F.req = -1; F.st.steals++; }   /* not taken yet: write it again */
		else {
			b = -1;
			for (i = 1; i < R.nbuf; i++) {
				int c = (R.shown + i) % R.nbuf;
				if (c != F.flipping && c_target_ready(c)) { b = c; break; }
			}
		}
		Permit();
		if (b < 0) {
			F.st.drops++;
			rtgc_skip(&R.conv, dirty);        /* nothing free: the lines stay owed */
			R.st.us_flip_wait += ticks_us(eclock() - t0);
			return RTG_OK;
		}
	} else if (R.nbuf > 1 && !R.degraded) {
		b = (R.shown + 1) % R.nbuf;
		if (!wait_msgs(c_target_ready, b, FLIP_TIMEOUT_US)) {
			/* messages did not come back: keep writing the buffer on display, no more flips */
			logf_(0, "rtgfw: screen buffer messages late (>100 ms): single buffer from now on");
			R.st.flips_refused++;
			R.degraded = 1;
		}
	}
	if (R.nbuf == 1) b = 0;
	else if (R.degraded && !F.task) b = R.shown;
	t1 = eclock();
	R.st.us_flip_wait += ticks_us(t1 - t0);

	if (R.blit) bm = R.offbm;
	else if (R.nbuf > 1) bm = R.sb[b]->sb_BitMap;
	else bm = R.scr->RastPort.BitMap;

	{
		ULONG lw = R.conv.lines_written, bw = R.conv.bytes_written;
		ULONG sv = surround_value(b);
		int cb = b, filled = 0;              /* converter buffer index = screen buffer index */
		if (sv != R.surround_val[cb]) R.surround_need |= (UBYTE)(1 << cb);
		if (R.oscopy) {
			struct RastPort rp;
			InitRastPort(&rp);
			rp.BitMap = bm;
			t2 = eclock();
			err = rtgc_present(&R.conv, cb, pix, stride, dirty, lineflags, R.staging, R.staging_bpr, &lo, &hi);
			if (R.surround_need & (1 << cb)) {
				/* the surround is painted through a lock even here (once per border change) */
				UBYTE *addr; LONG bpr;
				APTR h = lock_bm(bm, &addr, &bpr);
				if (h) { fill_surround(addr, bpr, sv); unlock_bm(bm, h); filled = 1; }
				R.surround_need &= (UBYTE)~(1 << cb); R.surround_val[cb] = sv;
			}
			if (err == RTG_OK) os_copy_spans(&rp);
			t3 = eclock();
			R.st.us_convert += ticks_us(t3 - t2);
		} else {
			UBYTE *addr = NULL;
			LONG bpr = 0;
			APTR h;
			t2 = eclock();
			h = lock_bm(bm, &addr, &bpr);
			t3 = eclock();
			R.st.us_lock += ticks_us(t3 - t2);
			if (!h || !addr) {
				if (h) unlock_bm(bm, h);
				R.errs++;
				/* frame skipped: its dirty lines stay owed here (the caller may clear
				 * its own marks after any present) and its runs are dropped */
				rtgc_skip(&R.conv, dirty);
				return RTG_E_LOCK;
			}
			if (R.surround_need & (1 << cb)) {
				fill_surround(addr, bpr, sv);
				R.surround_need &= (UBYTE)~(1 << cb); R.surround_val[cb] = sv;
				filled = 1;
			}
			err = rtgc_present(&R.conv, cb, pix, stride, dirty, lineflags, addr + (long)R.oy * bpr + (long)R.ox * R.bpp, bpr, &lo, &hi);
			t2 = eclock();
			unlock_bm(bm, h);
			t1 = eclock();
			R.st.us_convert += ticks_us(t2 - t3);
			R.st.us_lock += ticks_us(t1 - t2);
		}
		R.st.lines_written += R.conv.lines_written - lw;
		R.st.bytes_written += R.conv.bytes_written - bw;
		if (err == RTG_OK && R.blit) {
			if (filled) BltBitMapRastPort(R.offbm, 0, 0, &R.scr->RastPort, 0, 0, R.scr_w, R.scr_h, 0xc0);
			else blit_spans();
		}
	}
	if (err != RTG_OK) return err;
	R.last = b;
	if (F.task) {
		if (R.fmt == RTG_FMT_CLUT8) {
			int i;
			for (i = lo; i <= hi; i++) F.cur[i] = rtgc_clut_colour(&R.conv, i);
			if (lo <= hi) R.st.palette_loads++;
		}
		Forbid();
		if (R.fmt == RTG_FMT_CLUT8) memcpy(F.snap[b], F.cur, sizeof F.cur);
		F.req = b;
		F.st.queued++;
		Permit();
		Signal(F.task, F.go_mask);
		R.st.frames++;
		return RTG_OK;
	}
	if (R.fmt == RTG_FMT_CLUT8) load_clut(lo, hi);
	if (R.nbuf > 1 && !R.degraded) {
		if (ChangeScreenBuffer(R.scr, R.sb[b])) {
			R.safe_out[b] = R.disp_out[b] = 1;
			R.prev_of[b] = R.shown;
			R.writable[R.shown] = 0;
			R.writable[b] = 0;
			R.shown = b;
		} else R.st.flips_refused++;     /* written but not shown: it is the target again next time */
	}
	R.st.frames++;
	return RTG_OK;
}

int32_t rtg_present(RTG_REG(const uint8_t *, pixels, a0), RTG_REG(int32_t, stride, d0), RTG_REG(const uint8_t *, dirty, a1))
{
	return present_common(pixels, stride, dirty, NULL);
}

int32_t rtg_present_ex(RTG_REG(const RtgFrame *, f, a0))
{
	int i, err;
	if (!R.open) return RTG_E_NOTOPEN;
	if (!f || (f->nruns && (!f->runs || !f->pool)) || (f->colfmt != RTG_RGB24 && f->colfmt != RTG_RGB12)) return RTG_E_ARGS;
	for (i = 0; i < f->nruns; i++) {
		const RtgPalRun *r = &f->runs[i];
		int line = (int)r->line - R.vis_y;
		const void *col = f->colfmt == RTG_RGB12 ? (const void *)((const uint16_t *)f->pool + r->offset)
		                                         : (const void *)((const uint32_t *)f->pool + r->offset);
		if (line >= R.vis_h) break;
		if ((err = rtgc_add_run(&R.conv, line < 0 ? 0 : line, r->first, r->count_m1 + 1, col, f->colfmt)) != RTG_OK) {
			R.conv.nruns = 0; R.conv.pool_used = 0;
			return err;
		}
	}
	return present_common(f->pixels, f->stride, f->dirty, f->lineflags);
}

/* ------------------------------------------------------------------ */
/* bench extensions                                                    */

int32_t rtg_x_lock_cost(int n, uint32_t *us)
{
	struct BitMap *bm;
	int i;
	u64 t0;
	if (!R.open) return RTG_E_NOTOPEN;
	bm = R.blit ? R.offbm : (R.nbuf > 1 ? R.sb[R.last]->sb_BitMap : R.scr->RastPort.BitMap);
	t0 = eclock();
	for (i = 0; i < n; i++) {
		UBYTE *a; LONG b;
		APTR h = lock_bm(bm, &a, &b);
		if (!h) return RTG_E_LOCK;
		unlock_bm(bm, h);
	}
	if (us) *us = ticks_us(eclock() - t0);
	return RTG_OK;
}

int32_t rtg_x_lut_copy(const uint8_t *pix, int32_t stride, uint32_t *us)
{
	static ULONG ctab[256];
	struct RastPort rp;
	struct BitMap *bm;
	int i;
	u64 t0;
	if (!R.open) return RTG_E_NOTOPEN;
	if (!rtg_CyberGfxBase) return RTG_E_NOLIB;
	if (R.sx != 1 || R.sy != 1) return RTG_E_ARGS;
	bm = R.blit ? R.offbm : (R.nbuf > 1 ? R.sb[R.shown]->sb_BitMap : R.scr->RastPort.BitMap);
	for (i = 0; i < 256; i++) ctab[i] = R.conv.base[i];
	InitRastPort(&rp);
	rp.BitMap = bm;
	t0 = eclock();
	WriteLUTPixelArray((APTR)(pix + (long)R.vis_y * stride + R.vis_x), 0, 0, stride, &rp, ctab, R.ox, R.oy, R.vis_w, R.vis_h, CTABFMT_XRGB8);
	if (us) *us = ticks_us(eclock() - t0);
	return RTG_OK;
}

/* The whole screen of the buffer last written, in one lock: CLUT8 screens
 * give their pen numbers in pens[] (w x h, w = out_w) and the CLUT in
 * clut[256] (0x00RRGGBB); other formats give RTG_E_FORMAT. For grabs that
 * check what the screen really holds. */
int32_t rtg_x_read_frame(uint8_t *pens, uint32_t *clut)
{
	struct BitMap *bm;
	UBYTE *a;
	LONG bpr;
	APTR h;
	int y, i;
	if (!R.open) return RTG_E_NOTOPEN;
	if (R.fmt != RTG_FMT_CLUT8) return RTG_E_FORMAT;
	bm = R.blit ? R.scr->RastPort.BitMap : (R.nbuf > 1 ? R.sb[R.last]->sb_BitMap : R.scr->RastPort.BitMap);
	if (!(h = lock_bm(bm, &a, &bpr))) return RTG_E_LOCK;
	for (y = 0; y < R.scr_h; y++) memcpy(pens + (long)y * R.scr_w, a + (long)y * bpr, R.scr_w);
	unlock_bm(bm, h);
	if (clut) for (i = 0; i < 256; i++) clut[i] = rtgc_clut_colour(&R.conv, i);
	return RTG_OK;
}

/* The whole shown screen as 0x00RRGGBB (out_w x out_h longs), any format, one lock. */
int32_t rtg_x_read_rgb(uint32_t *rgb)
{
	struct BitMap *bm;
	UBYTE *a;
	LONG bpr;
	APTR h;
	int x, y;
	if (!R.open) return RTG_E_NOTOPEN;
	bm = R.blit ? R.scr->RastPort.BitMap : (R.nbuf > 1 ? R.sb[R.last]->sb_BitMap : R.scr->RastPort.BitMap);
	if (!(h = lock_bm(bm, &a, &bpr))) return RTG_E_LOCK;
	for (y = 0; y < R.scr_h; y++) {
		const UBYTE *p = a + (long)y * bpr;
		uint32_t *o = rgb + (long)y * R.scr_w, w;
		for (x = 0; x < R.scr_w; x++, p += R.bpp) {
			switch (R.fmt) {
			case RTG_FMT_CLUT8: o[x] = rtgc_clut_colour(&R.conv, p[0]); break;
			case RTG_FMT_RGB565: w = (p[0] << 8) | p[1]; o[x] = (((w >> 11) & 31) << 19) | (((w >> 5) & 63) << 10) | ((w & 31) << 3); break;
			case RTG_FMT_RGB565PC: w = (p[1] << 8) | p[0]; o[x] = (((w >> 11) & 31) << 19) | (((w >> 5) & 63) << 10) | ((w & 31) << 3); break;
			case RTG_FMT_ARGB32: o[x] = ((ULONG)p[1] << 16) | (p[2] << 8) | p[3]; break;
			case RTG_FMT_BGRA32: o[x] = ((ULONG)p[2] << 16) | (p[1] << 8) | p[0]; break;
			case RTG_FMT_RGBA32: o[x] = ((ULONG)p[0] << 16) | (p[1] << 8) | p[2]; break;
			default: o[x] = 0;
			}
		}
	}
	unlock_bm(bm, h);
	return RTG_OK;
}

uint32_t rtg_x_scan_info(uint32_t *high_pen_lines)
{
	if (high_pen_lines) *high_pen_lines = R.open ? R.conv.high_lines : 0;
	return R.open ? (uint32_t)R.conv.scan : 0;
}

int32_t rtg_x_read_pixel(int x, int y, uint32_t *rgb)
{
	struct BitMap *bm;
	UBYTE *a, *p;
	LONG bpr;
	APTR h;
	ULONG v = 0, w;
	if (!R.open) return RTG_E_NOTOPEN;
	if (x < 0 || y < 0 || x >= R.scr_w || y >= R.scr_h) return RTG_E_ARGS;
	bm = R.blit ? R.offbm : (R.nbuf > 1 ? R.sb[R.last]->sb_BitMap : R.scr->RastPort.BitMap);
	if (!(h = lock_bm(bm, &a, &bpr))) return RTG_E_LOCK;
	p = a + (long)y * bpr + (long)x * R.bpp;
	switch (R.fmt) {
	case RTG_FMT_CLUT8: v = rtgc_clut_colour(&R.conv, p[0]); break;
	case RTG_FMT_RGB565: w = (p[0] << 8) | p[1]; v = (((w >> 11) & 31) << 19) | (((w >> 5) & 63) << 10) | ((w & 31) << 3); break;
	case RTG_FMT_RGB565PC: w = (p[1] << 8) | p[0]; v = (((w >> 11) & 31) << 19) | (((w >> 5) & 63) << 10) | ((w & 31) << 3); break;
	case RTG_FMT_ARGB32: v = ((ULONG)p[1] << 16) | (p[2] << 8) | p[3]; break;
	case RTG_FMT_BGRA32: v = ((ULONG)p[2] << 16) | (p[1] << 8) | p[0]; break;
	case RTG_FMT_RGBA32: v = ((ULONG)p[0] << 16) | (p[1] << 8) | p[2]; break;
	}
	unlock_bm(bm, h);
	if (rgb) *rgb = v;
	return RTG_OK;
}
