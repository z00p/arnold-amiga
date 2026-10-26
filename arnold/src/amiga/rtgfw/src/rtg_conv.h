/* rtg_conv.h - the portable part of rtg_present(): picks the lines that must
 * be written into a buffer and converts them (8-bit copy, or palette lookup
 * into 16/32-bit pixels), plus the screen layout (scale and centring).
 * No AmigaOS calls: the Amiga side locks the bitmap, passes its address and
 * bytes per row, and flips. Host-tested by tests/test_conv.c.
 *
 * v0 (26 Oct 2026) additions over the design prototype: integer scaling
 * (sx, sy), RTG_L_BLANK lines in the border pen, runs at line 0 (per frame),
 * a per-line "written" output for the OS-copy and blit paths, and word/long
 * stores on big-endian hosts (the Amiga). */
#ifndef RTG_CONV_H
#define RTG_CONV_H

#include <stdint.h>
#include "rtgfw.h"

#define RTGC_MAXBUF  3
#define RTGC_MAXRUNS 64
#define RTGC_MAXPOOL (RTGC_MAXRUNS * 16 + 256)

typedef struct RtgConv {
	int w, h;                 /* visible size (source pixels/lines) */
	int fmt, bpp;             /* target format, bytes per pixel */
	int nbuf;
	int sx, sy;               /* integer scale (1 = none) */
	int border;               /* border pen (RTG_L_BLANK lines) */
	uint32_t base[256];       /* base palette, 0x00RRGGBB */
	uint32_t base_sig;
	int base_lo, base_hi;     /* changed range since the last present (CLUT load), lo > hi = none */
	uint8_t *need;            /* per line: bit b = buffer b must be rewritten */
	uint32_t *sig[RTGC_MAXBUF];  /* per buffer, per line: palette signature it was written with */
	uint32_t lut[256];        /* palette in the target format (direct formats), memory order */
	uint8_t *written;         /* optional (caller-owned, h bytes): set per line by rtgc_present */
	/* runs of the frame being built */
	int nruns;
	RtgPalRun runs[RTGC_MAXRUNS];
	uint32_t pool[RTGC_MAXPOOL];
	int pool_used;
	/* counters */
	uint32_t lines_written, bytes_written;
	/* scanlines (RTG_SCAN_*, see rtgfw.h; set with rtgc_set_scan after rtgc_init) */
	int scan, scan_pct;
	uint8_t dmap[256];        /* CLUT8 hard: the pen of each pen's dimmed colour (p | 128 for p < 128) */
	uint32_t lut_dim[256];    /* direct hard: dimmed colours, memory order */
	uint32_t lut_ha[256], lut_hb[256];  /* direct blend: halves (hb without alpha; RGB565PC: native order) */
	uint32_t high_lines;      /* CLUT8 hard: lines written with a pen >= 128 (shown undimmed/wrong) */
	uint32_t tab_sig;         /* base palette signature the direct-format tables were built from */
	int tab_runs;             /* the tables must be rebuilt (runs changed them, or scan changed) */
} RtgConv;

/* scan RTG_SCAN_OFF/HARD/BLEND, pct = darkness 0-100. CLUT8 + BLEND is refused
 * (RTG_E_FORMAT). The caller sets sy even (the lower sy/2 rows of each line
 * are the scanline). Everything is owed again. */
int  rtgc_set_scan(RtgConv *c, int scan, int pct);
/* the colour CLUT entry i must hold (base, or a dimmed base colour in CLUT8 hard mode) */
uint32_t rtgc_clut_colour(const RtgConv *c, int i);
/* per channel: dimmed (hard) and half-dimmed (blend) 0x00RRGGBB colours */
uint32_t rtgc_dim(uint32_t rgb, int pct);
uint32_t rtgc_half(uint32_t rgb, int pct);

int  rtgc_init(RtgConv *c, int w, int h, int fmt, int nbuf);
void rtgc_free(RtgConv *c);
/* every line owed to every buffer again (border pen change, buffer loss) */
void rtgc_invalidate(RtgConv *c);
/* a frame that could not be written (lock failed): its dirty lines stay owed
 * to every buffer, and its runs are dropped (they are per frame) */
void rtgc_skip(RtgConv *c, const uint8_t *dirty);
/* line <= 0: base palette (persistent); line > 0: a run for the next present */
int  rtgc_set_palette(RtgConv *c, int line, int first, int count, const void *col, int colfmt);
/* a run for the next present at any line, 0 included (rtg_present_ex) */
int  rtgc_add_run(RtgConv *c, int line, int first, int count, const void *col, int colfmt);
/* Write buffer `buf` (0..nbuf-1) at dst (bytes per row dbpr), from the
 * visible part of the chunky frame. dirty: per visible line, non-zero =
 * changed since the last call (any buffer). Line y lands at dst + y*sy*dbpr,
 * each pixel sx times, each line sy times. Returns RTG_OK or an error. For
 * CLUT8, pal_lo and pal_hi give the base palette range to load (lo > hi: none). */
int  rtgc_present(RtgConv *c, int buf, const uint8_t *pix, int stride, const uint8_t *dirty,
                  const uint8_t *lineflags, uint8_t *dst, int dbpr, int *pal_lo, int *pal_hi);
uint32_t rtgc_pack(int fmt, uint32_t rgb);   /* one colour in the target format */
int  rtgc_bpp(int fmt);
uint32_t rtgc_read_colour(const void *col, int colfmt, int i);   /* -> 0x00RRGGBB */

/* Layout: the biggest integer scale (keeping the pixel shape ax:ay) of
 * vis_w x vis_h source pixels on a scr_w x scr_h screen with square pixels,
 * centred. x/y negative = the picture is cropped. */
typedef struct RtgLayout { int sx, sy, x, y, w, h; } RtgLayout;
void rtgc_layout(int vis_w, int vis_h, int ax, int ay, int scr_w, int scr_h, RtgLayout *l);
/* the same with scanlines: an even scale (2x, 4x ...), or the 2x crop if even 2x does not fit */
void rtgc_layout_scan(int vis_w, int vis_h, int ax, int ay, int scr_w, int scr_h, RtgLayout *l);

#endif
