/* rtg_conv.c - portable core of rtg_present() (see rtg_conv.h).
 *
 * Which lines are written into buffer b this time:
 *   - lines the emulator marked dirty since the last present are owed to
 *     every buffer (need[y] = all buffer bits), and
 *   - in a direct (16/32-bit) format, any line whose effective palette
 *     signature differs from the one buffer b last wrote it with.
 * The signature of a line is a hash of the base palette plus every run that
 * starts at or above it, so a frame that repeats last frame's raster splits
 * costs nothing extra, while a changed split rewrites only the lines below it.
 *
 * On a big-endian host (the Amiga) the LUT holds each colour in the target's
 * memory order and pixels are stored as words or longs. On a little-endian
 * host (the PC test) they are stored byte by byte: same result in memory. */
#include <stdlib.h>
#include <string.h>
#include "rtg_conv.h"

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define RTGC_BE 1
#else
#define RTGC_BE 0
#endif

static uint32_t fnv(uint32_t h, uint32_t v)
{
	int i;
	for (i = 0; i < 4; i++) { h ^= (v >> (8 * i)) & 0xff; h *= 16777619u; }
	return h;
}

int rtgc_bpp(int fmt)
{
	switch (fmt) {
	case RTG_FMT_CLUT8: return 1;
	case RTG_FMT_RGB565: case RTG_FMT_RGB565PC: return 2;
	case RTG_FMT_ARGB32: case RTG_FMT_BGRA32: case RTG_FMT_RGBA32: return 4;
	}
	return 0;
}

/* The colour as the bytes appear in memory, packed big-endian into the
 * low bpp bytes of the result. */
uint32_t rtgc_pack(int fmt, uint32_t rgb)
{
	uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff, w;
	switch (fmt) {
	case RTG_FMT_RGB565:   return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
	case RTG_FMT_RGB565PC: w = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); return ((w & 0xff) << 8) | (w >> 8);
	case RTG_FMT_ARGB32:   return 0xff000000u | (r << 16) | (g << 8) | b;
	case RTG_FMT_BGRA32:   return (b << 24) | (g << 16) | (r << 8) | 0xffu;
	case RTG_FMT_RGBA32:   return (r << 24) | (g << 16) | (b << 8) | 0xffu;
	}
	return 0;
}

static uint32_t base_signature(const RtgConv *c)
{
	uint32_t h = 2166136261u;
	int i;
	for (i = 0; i < 256; i++) h = fnv(h, c->base[i]);
	return h;
}

int rtgc_init(RtgConv *c, int w, int h, int fmt, int nbuf)
{
	int b;
	memset(c, 0, sizeof *c);
	c->bpp = rtgc_bpp(fmt);
	if (w <= 0 || h <= 0 || !c->bpp || nbuf < 1 || nbuf > RTGC_MAXBUF) return RTG_E_ARGS;
	c->w = w; c->h = h; c->fmt = fmt; c->nbuf = nbuf;
	c->sx = c->sy = 1;
	c->need = malloc(h);
	if (!c->need) return RTG_E_NOMEM;
	memset(c->need, (1 << nbuf) - 1, h);          /* first present writes everything */
	for (b = 0; b < nbuf; b++) {
		c->sig[b] = calloc(h, sizeof(uint32_t));
		if (!c->sig[b]) { rtgc_free(c); return RTG_E_NOMEM; }
	}
	c->base_sig = base_signature(c);
	c->base_lo = 0; c->base_hi = 255;
	c->tab_runs = 1;                              /* build the direct-format tables at the first present */
	return RTG_OK;
}

void rtgc_free(RtgConv *c)
{
	int b;
	free(c->need); c->need = NULL;
	for (b = 0; b < RTGC_MAXBUF; b++) { free(c->sig[b]); c->sig[b] = NULL; }
}

void rtgc_invalidate(RtgConv *c)
{
	if (c->need) memset(c->need, (1 << c->nbuf) - 1, c->h);
}

void rtgc_skip(RtgConv *c, const uint8_t *dirty)
{
	int y, all = (1 << c->nbuf) - 1;
	if (c->need)
		for (y = 0; y < c->h; y++)
			if (!dirty || dirty[y] || (c->scan == RTG_SCAN_BLEND && y + 1 < c->h && dirty[y + 1])) c->need[y] = (uint8_t)all;
	c->nruns = 0; c->pool_used = 0;
}

uint32_t rtgc_read_colour(const void *col, int colfmt, int i)
{
	if (colfmt == RTG_RGB12) {
		uint32_t v = ((const uint16_t *)col)[i];
		return ((v >> 8) & 15) * 0x110000u + ((v >> 4) & 15) * 0x1100u + (v & 15) * 0x11u;
	}
	return ((const uint32_t *)col)[i] & 0xffffff;
}

int rtgc_add_run(RtgConv *c, int line, int first, int count, const void *col, int colfmt)
{
	int i;
	if (first < 0 || count < 1 || first + count > 256) return RTG_E_ARGS;
	if (c->fmt == RTG_FMT_CLUT8) return RTG_E_RUNS;          /* one palette a frame on CLUT */
	if (line < 0) line = 0;
	if (c->nruns >= RTGC_MAXRUNS || c->pool_used + count > RTGC_MAXPOOL) return RTG_E_RUNS;
	if (c->nruns && c->runs[c->nruns - 1].line > line) return RTG_E_RUNS;   /* must be in line order */
	c->runs[c->nruns].line = (uint16_t)line;
	c->runs[c->nruns].first = (uint8_t)first;
	c->runs[c->nruns].count_m1 = (uint8_t)(count - 1);
	c->runs[c->nruns].offset = (uint32_t)c->pool_used;
	for (i = 0; i < count; i++) c->pool[c->pool_used++] = rtgc_read_colour(col, colfmt, i);
	c->nruns++;
	return RTG_OK;
}

int rtgc_set_palette(RtgConv *c, int line, int first, int count, const void *col, int colfmt)
{
	int i;
	if (first < 0 || count < 1 || first + count > 256) return RTG_E_ARGS;
	if (line <= 0) {
		int changed = 0;
		for (i = 0; i < count; i++) {
			uint32_t v = rtgc_read_colour(col, colfmt, i);
			if (c->base[first + i] != v) { c->base[first + i] = v; changed = 1; }
		}
		if (changed) {
			if (c->base_lo > c->base_hi) { c->base_lo = first; c->base_hi = first + count - 1; }
			else {
				if (first < c->base_lo) c->base_lo = first;
				if (first + count - 1 > c->base_hi) c->base_hi = first + count - 1;
			}
			c->base_sig = base_signature(c);
		}
		return RTG_OK;
	}
	return rtgc_add_run(c, line, first, count, col, colfmt);
}

#if !RTGC_BE
static void store(uint8_t *d, uint32_t v, int bpp)
{
	switch (bpp) {
	case 2: d[0] = (uint8_t)(v >> 8); d[1] = (uint8_t)v; break;
	default: d[0] = (uint8_t)(v >> 24); d[1] = (uint8_t)(v >> 16); d[2] = (uint8_t)(v >> 8); d[3] = (uint8_t)v; break;
	}
}
#endif

/* mode: 0 normal, 1 doubled (RTG_L_X2), 2 blank (border pen) */
#define PEN(s, mode, x) ((mode) == 2 ? (uint8_t)c->border : (s)[(mode) ? (x) >> 1 : (x)])

/* one row of the line in full colour, each pixel sx times */
static void normal_row(RtgConv *c, const uint8_t *s, uint8_t *d, int mode)
{
	int x, k, w = c->w, sx = c->sx, bpp = c->bpp;
	if (bpp == 1) {
		if (mode == 2) memset(d, c->border, w * sx);
		else if (mode == 0 && sx == 1) memcpy(d, s, w);
		else if (mode == 0 && sx == 2) { uint16_t *p = (uint16_t *)d; for (x = w; x; x--) *p++ = (uint16_t)(*s++ * 0x0101u); }
		else for (x = 0; x < w; x++) { uint8_t v = PEN(s, mode, x); for (k = 0; k < sx; k++) *d++ = v; }
	} else {
		const uint32_t *lut = c->lut;
#if RTGC_BE
		if (bpp == 2) {
			uint16_t *p = (uint16_t *)d;
			if (mode == 2) { uint16_t v = (uint16_t)lut[c->border]; for (x = w * sx; x; x--) *p++ = v; }
			else if (mode == 0 && sx == 1) for (x = w; x; x--) *p++ = (uint16_t)lut[*s++];
			else if (mode == 0 && sx == 2) { uint32_t *q = (uint32_t *)d; for (x = w; x; x--) { uint32_t v = lut[*s++] & 0xffff; *q++ = (v << 16) | v; } }
			else for (x = 0; x < w; x++) { uint16_t v = (uint16_t)lut[PEN(s, mode, x)]; for (k = 0; k < sx; k++) *p++ = v; }
		} else {
			uint32_t *p = (uint32_t *)d;
			if (mode == 2) { uint32_t v = lut[c->border]; for (x = w * sx; x; x--) *p++ = v; }
			else if (mode == 0 && sx == 1) for (x = w; x; x--) *p++ = lut[*s++];
			else if (mode == 0 && sx == 2) for (x = w; x; x--) { uint32_t v = lut[*s++]; *p++ = v; *p++ = v; }
			else for (x = 0; x < w; x++) { uint32_t v = lut[PEN(s, mode, x)]; for (k = 0; k < sx; k++) *p++ = v; }
		}
#else
		for (x = 0; x < w; x++) {
			uint32_t v = lut[PEN(s, mode, x)];
			for (k = 0; k < sx; k++, d += bpp) store(d, v, bpp);
		}
#endif
	}
}

static uint32_t swap16(uint32_t v) { return ((v & 0xff) << 8) | ((v >> 8) & 0xff); }

/* the scanline row: hard = the line dimmed; blend = the line and the next (s2) averaged and dimmed */
static void scan_row(RtgConv *c, const uint8_t *s, int mode, const uint8_t *s2, int mode2, uint8_t *d)
{
	int x, k, w = c->w, sx = c->sx, bpp = c->bpp;
	if (bpp == 1) {                      /* CLUT8: hard only */
		const uint8_t *m = c->dmap;
		unsigned acc = 0;
		if (mode == 0 && sx == 2) { uint16_t *p = (uint16_t *)d; for (x = w; x; x--) { unsigned v = *s++; acc |= v; *p++ = (uint16_t)(m[v] * 0x0101u); } }
		else for (x = 0; x < w; x++) { unsigned v = PEN(s, mode, x); acc |= v; v = m[v]; for (k = 0; k < sx; k++) *d++ = (uint8_t)v; }
		if (acc & 0x80) c->high_lines++;
		return;
	}
	if (c->scan == RTG_SCAN_HARD) {
		const uint32_t *lut = c->lut_dim;
#if RTGC_BE
		if (bpp == 2) {
			if (mode == 0 && sx == 2) { uint32_t *q = (uint32_t *)d; for (x = w; x; x--) { uint32_t v = lut[*s++] & 0xffff; *q++ = (v << 16) | v; } }
			else { uint16_t *p = (uint16_t *)d; for (x = 0; x < w; x++) { uint16_t v = (uint16_t)lut[PEN(s, mode, x)]; for (k = 0; k < sx; k++) *p++ = v; } }
		} else {
			uint32_t *p = (uint32_t *)d;
			if (mode == 0 && sx == 2) for (x = w; x; x--) { uint32_t v = lut[*s++]; *p++ = v; *p++ = v; }
			else for (x = 0; x < w; x++) { uint32_t v = lut[PEN(s, mode, x)]; for (k = 0; k < sx; k++) *p++ = v; }
		}
#else
		for (x = 0; x < w; x++) { uint32_t v = lut[PEN(s, mode, x)]; for (k = 0; k < sx; k++, d += bpp) store(d, v, bpp); }
#endif
	} else {                             /* blend: halves of the two lines added (no carries between channels) */
		const uint32_t *ha = c->lut_ha, *hb = c->lut_hb;
		int pc = c->fmt == RTG_FMT_RGB565PC;
#if RTGC_BE
		if (bpp == 2) {
			if (mode == 0 && mode2 == 0 && sx == 2 && !pc) { uint32_t *q = (uint32_t *)d; for (x = w; x; x--) { uint32_t v = (ha[*s++] + hb[*s2++]) & 0xffff; *q++ = (v << 16) | v; } }
			else if (mode == 0 && mode2 == 0 && sx == 2) { uint32_t *q = (uint32_t *)d; for (x = w; x; x--) { uint32_t v = swap16(ha[*s++] + hb[*s2++]); *q++ = (v << 16) | v; } }
			else { uint16_t *p = (uint16_t *)d; for (x = 0; x < w; x++) { uint32_t v = ha[PEN(s, mode, x)] + hb[PEN(s2, mode2, x)]; if (pc) v = swap16(v); for (k = 0; k < sx; k++) *p++ = (uint16_t)v; } }
		} else {
			uint32_t *p = (uint32_t *)d;
			if (mode == 0 && mode2 == 0 && sx == 2) for (x = w; x; x--) { uint32_t v = ha[*s++] + hb[*s2++]; *p++ = v; *p++ = v; }
			else for (x = 0; x < w; x++) { uint32_t v = ha[PEN(s, mode, x)] + hb[PEN(s2, mode2, x)]; for (k = 0; k < sx; k++) *p++ = v; }
		}
#else
		for (x = 0; x < w; x++) {
			uint32_t v = ha[PEN(s, mode, x)] + hb[PEN(s2, mode2, x)];
			if (pc) v = swap16(v);
			for (k = 0; k < sx; k++, d += bpp) store(d, v, bpp);
		}
#endif
	}
}

/* The usual scanline cases at 2x2 in one pass, long stores, both rows at once (A1200, 26 Oct 2026:
 * the per-pixel word loops cost 0.76 ms a Sonic frame). CLUT8 hard: dmap[p] == p | 128 for every
 * pen, so the scanline row is the normal row with bit 7 set in each byte. Returns 0 if not handled.
 * 16/32-bit keep the row-by-row loops (normal_row, scan_row). */
#if RTGC_BE
#define PAIR8(a, b) (((uint32_t)(a) * 0x01010000u) | ((uint32_t)(b) * 0x0101u))   /* memory: a a b b */
#else
#define PAIR8(a, b) (((uint32_t)(b) * 0x01010000u) | ((uint32_t)(a) * 0x0101u))
#endif
static int fused_2x2(RtgConv *c, const uint8_t *s, uint8_t *d, int dbpr, int mode)
{
	int x, w = c->w;
	if (c->sx != 2 || c->sy != 2 || mode != 0) return 0;
	if (c->bpp == 1 && c->scan == RTG_SCAN_HARD && !(w & 1)) {
		/* two passes, one row after the other: the board takes sequential writes best (32-bit
		 * blend with both rows interleaved measured 1.11 ms against 0.81 ms in two passes) */
		const uint8_t *s0 = s;
		uint32_t *p = (uint32_t *)d, acc = 0;
		for (x = w >> 1; x; x--) { uint32_t v = PAIR8(s[0], s[1]); s += 2; acc |= v; *p++ = v; }
		p = (uint32_t *)(d + dbpr); s = s0;
		for (x = w >> 1; x; x--) { *p++ = PAIR8(s[0], s[1]) | 0x80808080u; s += 2; }
		if (acc & 0x80808080u) c->high_lines++;
		return 1;
	}
	return 0;
}

/* all rows of source line s (rows 0..sy-1 at d); s2/mode2 = the next line (blend) */
static void write_line(RtgConv *c, const uint8_t *s, uint8_t *d, int dbpr, int mode, const uint8_t *s2, int mode2)
{
	int k, sy = c->sy, n = c->w * c->sx * c->bpp, nrm = c->scan ? sy >> 1 : sy;
	if (c->scan && fused_2x2(c, s, d, dbpr, mode)) {
		c->lines_written++;
		c->bytes_written += (uint32_t)(n * sy);
		return;
	}
	if (nrm < 1) nrm = 1;
	normal_row(c, s, d, mode);
	for (k = 1; k < nrm; k++) memcpy(d + (long)k * dbpr, d, n);
	if (nrm < sy) {
		uint8_t *e = d + (long)nrm * dbpr;
		scan_row(c, s, mode, s2, mode2, e);
		for (k = nrm + 1; k < sy; k++) memcpy(d + (long)k * dbpr, e, n);
	}
	c->lines_written++;
	c->bytes_written += (uint32_t)(n * sy);
}

uint32_t rtgc_dim(uint32_t rgb, int pct)
{
	uint32_t f = (uint32_t)(100 - pct), r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
	return ((r * f / 100) << 16) | ((g * f / 100) << 8) | (b * f / 100);
}

uint32_t rtgc_half(uint32_t rgb, int pct)
{
	uint32_t f = (uint32_t)(100 - pct), r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
	return ((r * f / 200) << 16) | ((g * f / 200) << 8) | (b * f / 200);
}

int rtgc_set_scan(RtgConv *c, int scan, int pct)
{
	int i;
	if (scan < RTG_SCAN_OFF || scan > RTG_SCAN_BLEND || pct < 0 || pct > 100) return RTG_E_ARGS;
	if (scan == RTG_SCAN_BLEND && c->fmt == RTG_FMT_CLUT8) return RTG_E_FORMAT;
	c->scan = scan; c->scan_pct = pct; c->high_lines = 0; c->tab_runs = 1;
	for (i = 0; i < 256; i++) c->dmap[i] = (uint8_t)(scan && i < 128 ? i | 128 : i);
	if (c->fmt == RTG_FMT_CLUT8 && scan) { c->base_lo = 0; c->base_hi = 255; }   /* load the dimmed half */
	rtgc_invalidate(c);
	return RTG_OK;
}

uint32_t rtgc_clut_colour(const RtgConv *c, int i)
{
	if (c->scan == RTG_SCAN_HARD && c->fmt == RTG_FMT_CLUT8 && i >= 128) return rtgc_dim(c->base[i - 128], c->scan_pct);
	return c->base[i & 255];
}

/* the direct-format tables for entry i from colour v */
static void set_entry(RtgConv *c, int i, uint32_t v)
{
	c->lut[i] = rtgc_pack(c->fmt, v);
	if (c->scan == RTG_SCAN_HARD) c->lut_dim[i] = rtgc_pack(c->fmt, rtgc_dim(v, c->scan_pct));
	else if (c->scan == RTG_SCAN_BLEND) {
		uint32_t h = rtgc_half(v, c->scan_pct);
		if (c->fmt == RTG_FMT_RGB565PC) c->lut_ha[i] = c->lut_hb[i] = rtgc_pack(RTG_FMT_RGB565, h);
		else { c->lut_ha[i] = rtgc_pack(c->fmt, h); c->lut_hb[i] = c->lut_ha[i] & ~rtgc_pack(c->fmt, 0); }   /* alpha once */
	}
}

int rtgc_present(RtgConv *c, int buf, const uint8_t *pix, int stride, const uint8_t *dirty,
                 const uint8_t *lineflags, uint8_t *dst, int dbpr, int *pal_lo, int *pal_hi)
{
	int y, ri = 0, i, all = (1 << c->nbuf) - 1, bit = 1 << buf, blend = c->scan == RTG_SCAN_BLEND;
	uint32_t sig = c->base_sig;
	long rowstep = (long)dbpr * c->sy;
	if (buf < 0 || buf >= c->nbuf) return RTG_E_ARGS;
	if (c->fmt == RTG_FMT_CLUT8) {
		*pal_lo = c->base_lo; *pal_hi = c->base_hi;
		if (c->scan == RTG_SCAN_HARD && c->base_lo <= c->base_hi && c->base_lo < 128) {
			int h2 = (c->base_hi < 128 ? c->base_hi : 127) + 128;   /* and their dimmed copies */
			if (h2 > *pal_hi) *pal_hi = h2;
		}
	} else {
		*pal_lo = 1; *pal_hi = 0;
		if (c->tab_sig != c->base_sig || c->tab_runs) {
			for (i = 0; i < 256; i++) set_entry(c, i, c->base[i]);
			c->tab_sig = c->base_sig;
		}
		c->tab_runs = c->nruns != 0;      /* runs change the tables while the frame is drawn */
	}
	c->base_lo = 1; c->base_hi = 0;
	for (y = 0; y < c->h; y++) {
		while (ri < c->nruns && c->runs[ri].line <= y) {
			const RtgPalRun *r = &c->runs[ri];
			int n = r->count_m1 + 1;
			sig = fnv(fnv(sig, ((uint32_t)r->first << 8) | r->count_m1), 0x52554e53u);
			for (i = 0; i < n; i++) {
				uint32_t v = c->pool[r->offset + i];
				set_entry(c, r->first + i, v);
				sig = fnv(sig, v);
			}
			ri++;
		}
		if (!dirty || dirty[y] || (blend && y + 1 < c->h && dirty[y + 1])) c->need[y] = (uint8_t)all;
		if ((c->need[y] & bit) || (c->fmt != RTG_FMT_CLUT8 && c->sig[buf][y] != sig)) {
			int f = lineflags ? lineflags[y] : 0, y2 = y + 1 < c->h ? y + 1 : y;
			int f2 = lineflags ? lineflags[y2] : 0;
			write_line(c, pix + (long)y * stride, dst + y * rowstep, dbpr,
			           (f & RTG_L_BLANK) ? 2 : (f & RTG_L_X2) ? 1 : 0,
			           pix + (long)y2 * stride, (f2 & RTG_L_BLANK) ? 2 : (f2 & RTG_L_X2) ? 1 : 0);
			c->need[y] &= (uint8_t)~bit;
			c->sig[buf][y] = sig;
			if (c->written) c->written[y] = 1;
		} else if (c->written) c->written[y] = 0;
	}
	c->nruns = 0; c->pool_used = 0;   /* runs are per frame */
	return RTG_OK;
}

void rtgc_layout(int vis_w, int vis_h, int ax, int ay, int scr_w, int scr_h, RtgLayout *l)
{
	int k = 0;
	if (ax < 1) ax = 1;
	if (ay < 1) ay = 1;
	while (vis_w * ax * (k + 1) <= scr_w && vis_h * ay * (k + 1) <= scr_h) k++;
	if (k == 0) {
		/* does not fit at its shape: show 1:1 and crop the middle */
		ax = ay = 1; k = 1;
	}
	l->sx = ax * k; l->sy = ay * k;
	l->w = vis_w * l->sx; l->h = vis_h * l->sy;
	l->x = (scr_w - l->w) / 2; l->y = (scr_h - l->h) / 2;   /* negative = cropped */
}

void rtgc_layout_scan(int vis_w, int vis_h, int ax, int ay, int scr_w, int scr_h, RtgLayout *l)
{
	if (ax < 1) ax = 1;
	if (ay < 1) ay = 1;
	rtgc_layout(vis_w, vis_h, 2 * ax, 2 * ay, scr_w, scr_h, l);
	if (l->sy == 1) {                       /* rtgc_layout's crop fallback is 1:1: scanlines need 2x */
		l->sx = 2 * ax; l->sy = 2 * ay; l->w = vis_w * l->sx; l->h = vis_h * l->sy;
		l->x = (scr_w - l->w) / 2; l->y = (scr_h - l->h) / 2;
	}
}
