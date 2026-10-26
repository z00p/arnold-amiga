/* rtgfw.h - common RTG display framework for John's Amiga emulators.
 *
 * v0 (26 Oct 2026): implemented in rtgfw.c (amiga-gcc, m68k-amigaos) and
 * measured by bench/rtgbench.c. Not used by any emulator yet. See
 * ../DESIGN.md for the design and ../README.md for the status.
 *
 * ABI: every entry point takes its arguments in registers, Amiga-library
 * style, so vasm code can call it without a C stack frame:
 *   arguments  d0-d3 / a0-a1 as listed per function
 *   result     d0 (error code, value or pointer)
 *   scratch    d0, d1, a0, a1 (trashed); everything else is preserved
 * amiga-gcc builds the same prototypes with register parameters (RTG_REG),
 * so C callers simply call the functions. On other hosts RTG_REG is a plain
 * parameter (the PC test build).
 *
 * Task context only: call from the emulator task with a normal A7 stack,
 * never from an interrupt or from inside a CPU core.
 *
 * One instance per program: the framework keeps its state internally, so no
 * handle is passed around (simpler for assembler callers).
 */
#ifndef RTGFW_H
#define RTGFW_H

#include <stdint.h>

#if defined(__amigaos__) && defined(__mc68000__)
#define RTG_REG(type, name, reg) register type name __asm(#reg)
#else
#define RTG_REG(type, name, reg) type name
#endif

#define RTGFW_API 1

/* ---- error codes (d0). 0 is success everywhere. ---- */
enum {
	RTG_OK = 0,
	RTG_E_NOLIB,      /* neither Picasso96API.library 2+ nor cybergraphics.library 41+ */
	RTG_E_NOMODE,     /* no RTG mode with the wanted depth and at least the wanted size */
	RTG_E_NOMEM,
	RTG_E_SCREEN,     /* OpenScreen / OpenWindow failed */
	RTG_E_BUFFERS,    /* AllocScreenBuffer failed (the caller may retry with buffers = 1) */
	RTG_E_FORMAT,     /* the bitmap's pixel format is not one the converter writes */
	RTG_E_ARGS,       /* bad sizes, struct size or API version */
	RTG_E_NOTOPEN,
	RTG_E_RUNS,       /* palette runs: out of order, too many, or runs on an 8-bit screen */
	RTG_E_LOCK,       /* LockBitMap failed (bitmap not in board memory) */
	RTG_E_COUNT
};

/* ---- target pixel formats (what the RTG bitmap really is) ---- */
enum {
	RTG_FMT_NONE = 0,
	RTG_FMT_CLUT8,       /* 8-bit, hardware palette */
	RTG_FMT_RGB565,      /* 16-bit big-endian (P96 RGBFB_R5G6B5, CGX PIXFMT_RGB16) */
	RTG_FMT_RGB565PC,    /* 16-bit little-endian (RGBFB_R5G6B5PC, PIXFMT_RGB16PC) */
	RTG_FMT_ARGB32,      /* RGBFB_A8R8G8B8, PIXFMT_ARGB32 */
	RTG_FMT_BGRA32,      /* RGBFB_B8G8R8A8, PIXFMT_BGRA32 (the A1200 Workbench mode) */
	RTG_FMT_RGBA32,      /* RGBFB_R8G8B8A8, PIXFMT_RGBA32 */
	RTG_FMT_COUNT
};

/* ---- colour input formats for rtg_set_palette ---- */
#define RTG_RGB24  0     /* ULONG 0x00RRGGBB per colour */
#define RTG_RGB12  1     /* UWORD 0x0RGB per colour (an Amiga 12-bit colour word); expanded x17 */

/* ---- open flags ---- */
#define RTG_F_PERLINE  0x0001  /* frames may carry palette runs: needs a 16/32-bit screen */
#define RTG_F_WINDOW   0x0002  /* later: a window on the Workbench screen instead of a screen */
#define RTG_F_OSCOPY   0x0004  /* copy with WriteChunkyPixels / WritePixelArray, never lock */
#define RTG_F_NOHWSCALE 0x0008 /* never rely on the board's scaler; use a big mode + integer scale */
#define RTG_F_LOG      0x0010  /* call log() with one line per stage */
#define RTG_F_BLIT     0x0020  /* v0: no screen buffers; write an off-screen bitmap, blit the changed lines */

/* ---- scanlines (RtgOpen.scan; added 26 Oct 2026). The picture is drawn at
 * an even integer scale (2x, 4x); in each source line's group of output rows
 * the lower half is the "scanline":
 *   RTG_SCAN_HARD   a copy of the line, each channel * (100 - scan_pct) / 100.
 *                   CLUT8: pens 0-127 use CLUT entries 128-255 for their dimmed
 *                   colours (the framework loads them), so the frame must use
 *                   pens 0-127 only (pens 128+ are counted, rtg_x_scan_info).
 *   RTG_SCAN_BLEND  the line and the line below averaged, then dimmed:
 *                   each channel = h(upper) + h(lower), h(c) = c * (100 - scan_pct) / 200
 *                   (integer, per channel, in the target's precision); the last
 *                   line blends with itself. 16/32-bit screens only. With palette
 *                   runs, both pixels use the upper line's palette.
 * Scanlines only change how a frame is drawn: the dirty-line rules stay the
 * caller's (a changed line also redraws the blend row above it). ---- */
#define RTG_SCAN_OFF   0
#define RTG_SCAN_HARD  1
#define RTG_SCAN_BLEND 2

/* ---- per-line flags (RtgFrame.lineflags) ---- */
#define RTG_L_X2       0x01    /* line holds vis_w/2 pixels: double each one (MSX 256 lines in a 512 frame) */
#define RTG_L_BLANK    0x02    /* line is all border pen (not read) */
#define RTG_L_RGB16    0x04    /* reserved (not in v1): direct RGB565 line from a second pixel pointer (MSX YJK) */

struct Screen;
struct Window;

typedef struct RtgCaps {
	uint16_t size;            /* in: sizeof(RtgCaps) */
	uint16_t api;             /* out: RTGFW_API */
	uint8_t  system;          /* out: 0 none, 1 Picasso96, 2 CyberGraphX (or P96's CGX emulation only) */
	uint8_t  depths;          /* out: bit 0 = 8-bit CLUT, bit 1 = 16-bit, bit 2 = 32-bit modes exist */
	uint16_t lib_version;     /* out: version of the library used */
	char     board[32];       /* out: board/driver name, e.g. "VideoCore", "Minimig", "uaegfx" */
} RtgCaps;

typedef struct RtgOpen {
	uint16_t size, api;            /* sizeof(RtgOpen), RTGFW_API */
	uint16_t src_w, src_h;         /* chunky frame size (pixels, lines) */
	uint16_t vis_x, vis_y;         /* part of the frame to show ... */
	uint16_t vis_w, vis_h;         /* ... (e.g. Arnold 384x272 frame, 320x256 or 384x272 shown) */
	uint8_t  aspect_x, aspect_y;   /* shape of one source pixel: 1:1, or 1:2 for MSX 512-wide */
	uint8_t  depth;                /* 0 auto, 8, 16, 32 */
	uint8_t  scale;                /* 0 auto, else fixed integer scale */
	uint8_t  buffers;              /* 0 auto (3), 1 = single buffer (tears), 2, 3 */
	uint8_t  border_pen;           /* palette index that fills the surround (CPC/MSX border colour) */
	uint16_t flags;                /* RTG_F_* */
	uint32_t mode_id;              /* 0: choose (BestModeID); else this RTG mode */
	uint16_t content_hz100;        /* 5000 or 6000 (for the log and the mode choice) */
	uint8_t  scan;                 /* RTG_SCAN_*: 0 off; else the picture is drawn at 2x height (and 2x width) */
	uint8_t  scan_pct;             /* how much darker the in-between lines are, 0-100 (25 = 75% bright) */
	void   (*log)(const char *line);  /* optional, may be NULL */
	/* out */
	uint32_t out_mode_id;
	uint16_t out_w, out_h;         /* screen size */
	uint16_t out_x, out_y;         /* where the picture's top left lands */
	uint16_t out_scale_x, out_scale_y;
	uint8_t  out_fmt;              /* RTG_FMT_* */
	uint8_t  out_buffers;          /* 1-3 screen buffers; 0 = RTG_F_BLIT (off-screen bitmap + blit) */
	uint16_t reserved2;
	struct Screen *out_screen;     /* for menus/requesters on the same output */
	struct Window *out_window;     /* backdrop window: its UserPort carries IDCMP_RAWKEY etc. */
} RtgOpen;

/* A palette run: from line `line` (0 = top of the frame) to the end of the
 * frame, palette entries first..first+count-1 take these colours. Runs are
 * per frame: rtg_present() returns the palette to the base palette. */
typedef struct RtgPalRun {
	uint16_t line;
	uint8_t  first;
	uint8_t  count_m1;   /* count - 1 (0..255) */
	uint32_t offset;     /* index of the first colour in the frame's colour pool */
} RtgPalRun;

/* The full frame description (rtg_present_ex). rtg_present() is the
 * short form with no runs and no line flags. */
typedef struct RtgFrame {
	const uint8_t *pixels;        /* chunky 8-bit indices, src_w x src_h */
	int32_t        stride;        /* bytes from one line to the next */
	const uint8_t *dirty;         /* per source line: non-zero = changed since the last present; NULL = all */
	const uint8_t *lineflags;     /* per source line RTG_L_*, or NULL */
	uint16_t       nruns, colfmt; /* palette runs this frame; RTG_RGB24 / RTG_RGB12 for the pool */
	const RtgPalRun *runs;        /* sorted by line */
	const void    *pool;          /* the runs' colours */
} RtgFrame;

typedef struct RtgStats {
	uint32_t frames, lines_written, bytes_written, palette_loads;
	uint32_t us_convert, us_lock, us_flip_wait, flips_refused;
	uint32_t refresh_us;          /* measured time between flips' DispMessages (the RTG refresh) */
} RtgStats;

/* Function                                          registers           result d0 */
int32_t  rtg_query(RTG_REG(RtgCaps *, caps, a0));                 /* error */
int32_t  rtg_open(RTG_REG(RtgOpen *, o, a0));                        /* error; o->out_* filled */
void     rtg_close(void);
int32_t  rtg_set_palette(RTG_REG(int32_t , line, d0),              /* 0 = base palette */
                         RTG_REG(int32_t , first, d1),
                         RTG_REG(int32_t , count, d2),
                         RTG_REG(const void *, colours, a0),
                         RTG_REG(int32_t , colfmt, d3));         /* error */
int32_t  rtg_set_border(RTG_REG(int32_t , pen, d0));
int32_t  rtg_present(RTG_REG(const uint8_t *, pixels, a0),
                     RTG_REG(int32_t , stride, d0),
                     RTG_REG(const uint8_t *, dirty, a1));       /* error */
int32_t  rtg_present_ex(RTG_REG(const RtgFrame *, f, a0));
void     rtg_set_pacing(RTG_REG(uint32_t , frame_us, d0));    /* 0: off */
int32_t  rtg_wait_frame(void);                                        /* us behind schedule (0 = on time) */
const char *rtg_error_text(RTG_REG(int32_t , err, d0));
void     rtg_get_stats(RTG_REG(RtgStats *, s, a0));

/* LoadSeg module form (for AmiMSX): the first code hunk starts with
 *   moveq #-1,d0 / rts              (not a program)
 *   dc.l 'RTGF', RTGFW_API
 *   jmp table, 6 bytes an entry, in the order of RTG_JT_* below.
 * The caller LoadSegs PROGDIR:rtgfw.module, checks the magic and calls
 * entry n at (segment start + 4 + 8 + 6 * n). Not built in v0. */
enum { RTG_JT_QUERY, RTG_JT_OPEN, RTG_JT_CLOSE, RTG_JT_SET_PALETTE, RTG_JT_SET_BORDER,
       RTG_JT_PRESENT, RTG_JT_PRESENT_EX, RTG_JT_SET_PACING, RTG_JT_WAIT_FRAME,
       RTG_JT_ERROR_TEXT, RTG_JT_GET_STATS, RTG_JT_COUNT };

/* ---- v0 extensions for the bench and diagnostics: C only, plain stack
 * arguments, not in the jump table, may change. ---- */
typedef struct RtgModeInfo {
	uint32_t id;
	uint16_t w, h;
	uint8_t  fmt;        /* RTG_FMT_*, or RTG_FMT_NONE for formats rtgfw cannot write */
	uint8_t  depth;      /* bits per pixel */
	uint8_t  rawfmt;     /* P96 RGBFB_* (or CGX PIXFMT_* when P96 calls are not used) */
	uint8_t  pad;
	char     name[40];
} RtgModeInfo;

int         rtg_x_modes(RtgModeInfo *m, int max);       /* RTG modes; returns how many */
const char *rtg_x_info(void);                           /* system, versions, lock method, self-checks */
uint32_t    rtg_x_micros(void);                         /* E-clock time in microseconds (needs open or pacing) */
uint32_t    rtg_x_eclock_hz(void);
int         rtg_x_set_lock_method(int m);               /* 0 auto, 1 P96, 2 CGX; returns the method in use */
int32_t     rtg_x_lock_cost(int n, uint32_t *us);       /* n lock+unlock pairs on the target bitmap */
int32_t     rtg_x_sync_display(uint32_t timeout_us, uint32_t *waited_us); /* wait for every DispMessage */
int32_t     rtg_x_lut_copy(const uint8_t *pix, int32_t stride, uint32_t *us); /* one WriteLUTPixelArray frame (CGX) */
int32_t     rtg_x_read_pixel(int x, int y, uint32_t *rgb); /* read back the last written buffer, as 0x00RRGGBB */
int32_t     rtg_x_read_frame(uint8_t *pens, uint32_t *clut); /* CLUT8: the whole shown screen (out_w x out_h pens) + its CLUT */
int32_t     rtg_x_read_rgb(uint32_t *rgb);              /* any format: the whole shown screen as 0x00RRGGBB (out_w x out_h) */
uint32_t    rtg_x_scan_info(uint32_t *high_pen_lines);  /* scanline mode in use (RTG_SCAN_*); lines drawn with pens 128+ in CLUT8 hard mode */

#endif
