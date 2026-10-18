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
static const char ver[] __attribute__((used)) = "$VER: Arnold 0.5 (16.10.2026) Amiga port, GPL-2.0";

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
#define KQ 64
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

int Sys_OpenDisplay(int cw, int ch, int hires)
{
	struct Rectangle rect;
	ULONG modeid = PAL_MONITOR_ID | (hires ? HIRES_KEY : LORES_KEY);
	int i, rw, rh;

	CW = cw; CH = ch;
	c2p_init();
	quit_requested = 0; kq_head = kq_tail = 0;
	/* Standard overscan, not MAX: MAX's rectangle starts further left than a
	 * monitor shows (first WinUAE run: the left border was off the screen). */
	if (!QueryOverscan(modeid, &rect, OSCAN_STANDARD)) {
		rect.MinX = 0; rect.MinY = 0; rect.MaxX = (hires ? 703 : 351); rect.MaxY = 271;
	}
	rw = rect.MaxX - rect.MinX + 1;
	rh = rect.MaxY - rect.MinY + 1;
	SW = (rw < cw ? rw : cw) & ~31;
	SH = rh < ch ? rh : ch;
	Sys_Log("display: mode %08lx overscan %d,%d-%d,%d screen %dx%d chunky %dx%d chip free %lu", (unsigned long)modeid,
		(int)rect.MinX, (int)rect.MinY, (int)rect.MaxX, (int)rect.MaxY, SW, SH, cw, ch, (unsigned long)AvailMem(MEMF_CHIP));
	XOFF = (cw - SW) / 2;
	YOFF = (ch - SH) / 2;

	scr = OpenScreenTags(NULL,
		SA_Width, SW, SA_Height, SH, SA_Depth, 8,
		SA_DisplayID, modeid, SA_Overscan, OSCAN_STANDARD,
		SA_Left, (rw - SW) / 2, SA_Top, (rh - SH) / 2,
		SA_Quiet, TRUE, SA_ShowTitle, FALSE, SA_Title, (ULONG)"Arnold",
		SA_Interleaved, TRUE,
		TAG_DONE);
	if (!scr) { Sys_Log("display: OpenScreen failed"); return 0; }

	safeport = CreateMsgPort(); dispport = CreateMsgPort();
	if (!safeport || !dispport) { Sys_CloseDisplay(); return 0; }
	for (i = 0; i < NBUF; i++) {
		struct BitMap *sbm = scr->RastPort.BitMap;
		safe_pend[i] = disp_pend[i] = 0;
		own_bm[i] = NULL;
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
		{
			struct BitMap *b = sb[i]->sb_BitMap;
			Sys_Log("display: buffer %d %s bpr %d planes %08lx %08lx (+%ld) flags %08lx", i, own_bm[i] ? "own" : "system",
				(int)b->BytesPerRow, (unsigned long)b->Planes[0], (unsigned long)b->Planes[1],
				(long)(b->Planes[1] - b->Planes[0]), (unsigned long)GetBitMapAttr(b, BMA_FLAGS));
		}
		sb[i]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = safeport;
		sb[i]->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = dispport;
	}
	line_dirty = AllocMem(CH, MEMF_ANY);
	if (!line_dirty) { Sys_CloseDisplay(); return 0; }
	memset(line_dirty, ALL_BUFFERS, CH);   /* full first conversion into every buffer */
	for (i = 0; i < NBUF; i++) buf_safe[i] = (i != 0);
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
	Sys_Log("display: open");
	return 1;
}

static void collect_safe(void)
{
	struct Message *m;
	int j;
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
	while ((m = GetMsg(dispport))) {
		for (j = 0; j < NBUF; j++)
			if (sb[j] && m == &sb[j]->sb_DBufInfo->dbi_DispMessage) disp_pend[j] = 0;
		disp_out--;
	}
}

void Sys_CloseDisplay(void)
{
	int i;
	while (safe_out > 0) { WaitPort(safeport); collect_safe(); }
	while (disp_out > 0) { WaitPort(dispport); collect_disp(); }
	if (win) { ClearPointer(win); CloseWindow(win); win = NULL; }
	if (blankptr) { FreeVec(blankptr); blankptr = NULL; }
	for (i = 0; i < NBUF; i++) {
		if (sb[i] && scr) { FreeScreenBuffer(scr, sb[i]); sb[i] = NULL; }
		if (own_bm[i]) { WaitBlit(); FreeBitMap(own_bm[i]); own_bm[i] = NULL; }
	}
	if (line_dirty) { FreeMem(line_dirty, CH); line_dirty = NULL; }
	if (scr) { CloseScreen(scr); scr = NULL; }
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
	if (line_dirty && y >= 0 && y < CH) line_dirty[y] = ALL_BUFFERS;
}

void Sys_ShowFrame(const unsigned char *chunky, int pitch)
{
	unsigned long t0, t1;
	struct BitMap *bm;
	int y, next, bit;

	if (!scr) return;
	t0 = Sys_Micros();
	next = (shown + 1) % NBUF;
	collect_safe();
	if (!buf_safe[next]) {
		hb_safe_waits++;
		if (hb_log_waits) Sys_Log("wait: buffer %d not safe yet (shown %d, safe_out %d, disp_out %d)", next, shown, safe_out, disp_out);
	}
	while (!buf_safe[next]) { WaitPort(safeport); collect_safe(); }
	t1 = Sys_Micros(); Sys_Perf.wait_us += t1 - t0; Sys_Perf.wait_safe_us += t1 - t0; t0 = t1;

	bm = sb[next]->sb_BitMap;
	bit = 1 << next;
	for (y = 0; y < SH; y++) {
		const unsigned char *src = chunky + (y + YOFF) * pitch + XOFF;
		if (Sys_AllLines || (line_dirty[y + YOFF] & bit)) {
			line_dirty[y + YOFF] &= ~bit;
			c2p_line(src, bm, y);
			Sys_Perf.c2p_lines++;
		}
	}
	if (Sys_Osd) draw_osd(bm);
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
		safe_out++; disp_out++;
		safe_pend[next] = disp_pend[next] = 1;
	}
	frames++;
	t1 = Sys_Micros(); Sys_Perf.wait_us += t1 - t0; Sys_Perf.wait_disp_us += t1 - t0;
}

long Sys_WaitFrame(void)
{
	/* One CPC frame per PAL VBlank. On time: WaitTOF, and the next frame is due
	 * 20 ms after that VBlank. Late (13 Oct 2026: BigMig took 40-80 ms a frame):
	 * no wait, and the schedule is kept, so the debt shows how far behind the
	 * emulation is; the frame skip uses it to catch up. More than 10 frames
	 * behind: give up the debt and restart the schedule from now.
	 * Returns how far behind schedule this frame ended, in us (0 if on time). */
	static unsigned long due;
	long behind = 0;
	unsigned long t0 = Sys_Micros();
	collect_disp();
	if (due == 0 || (long)(due - t0) > 500) {
		WaitTOF();
		due = Sys_Micros() + 20000;
	} else {
		behind = (long)(t0 - due);
		if (behind > 200000) due = t0 + 20000;   /* still reported: the skip carries on */
		else due += 20000;
		Sys_Perf.late_frames++;
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
		if (msg->Class == IDCMP_RAWKEY && (msg->Code & 0x7f) < 0x78) {   /* 0x78+: mouse wheel and buttons, not keys */
			int next = (kq_tail + 1) % KQ;
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
static volatile unsigned long aud_underruns, aud_drops;
static struct Interrupt aud_vbint;
static int aud_vbint_added;

static LONG aud_vblank(register APTR data __asm("a1"))
{
	if (aud_run) {
		if (aud_count > 0) {
			custom.aud[0].ac_ptr = (UWORD *)aud_buf[0][aud_rd];
			custom.aud[1].ac_ptr = (UWORD *)aud_buf[1][aud_rd];
			aud_rd = (aud_rd + 1) % ABUFS;
			aud_count--;
		} else
			aud_underruns++;
	}
	return 0;
}

int Sys_AudioOpen(int period, int samples)
{
	int c, b;
	if (aud_open) return 1;
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
	aud_vbint.is_Node.ln_Type = NT_INTERRUPT;
	aud_vbint.is_Node.ln_Pri = 0;
	aud_vbint.is_Node.ln_Name = (char *)"Arnold audio";
	aud_vbint.is_Data = NULL;
	aud_vbint.is_Code = (VOID (*)())aud_vblank;
	AddIntServer(INTB_VERTB, &aud_vbint);
	aud_vbint_added = 1;
	Sys_Log("audio: open, period %d, %d samples a block", period, samples);
	return 1;
}

void Sys_AudioClose(void)
{
	int c, b;
	if (!aud_open) return;
	aud_run = 0;
	if (aud_vbint_added) { RemIntServer(INTB_VERTB, &aud_vbint); aud_vbint_added = 0; }
	custom.dmacon = DMAF_AUD0 | DMAF_AUD1;   /* clear */
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
	Disable();
	aud_wr = (aud_wr + 1) % ABUFS;
	aud_count++;
	if (!aud_run) {
		/* start: this block plays at once, the server feeds the rest */
		custom.aud[0].ac_ptr = (UWORD *)aud_buf[0][slot];
		custom.aud[1].ac_ptr = (UWORD *)aud_buf[1][slot];
		aud_rd = aud_wr; aud_count = 0;
		custom.dmacon = DMAF_SETCLR | DMAF_MASTER | DMAF_AUD0 | DMAF_AUD1;
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
	Enable();
	custom.dmacon = DMAF_AUD0 | DMAF_AUD1;
}

unsigned long Sys_Millis(void) { return frames * 20; }

void Sys_Message(const char *text)
{
	Sys_Log("message: %s", text);
	struct EasyStruct es = { sizeof(struct EasyStruct), 0, "Arnold", "%s", "OK" };
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
