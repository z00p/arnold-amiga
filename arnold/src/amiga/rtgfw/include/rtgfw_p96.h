/* rtgfw_p96.h - the few Picasso96API.library calls rtgfw uses.
 *
 * Own declarations, written from the public Picasso96API autodoc and fd
 * (function order and registers); not copied from the P96 SDK, which the
 * bebbo toolchain does not ship. Only what rtgfw needs is declared.
 *
 * The library base is P96Base_rtg (private to rtgfw, so it never clashes
 * with a program's own P96Base). rtgfw.c cross-checks the mode width that
 * p96GetModeIDAttr() returns against graphics.library before it trusts any
 * other P96 call, so a wrong offset here shows as "P96 calls disabled" in
 * the log instead of a crash later. */
#ifndef RTGFW_P96_H
#define RTGFW_P96_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <graphics/gfx.h>
#include <graphics/rastport.h>

extern struct Library *P96Base_rtg;

/* RGBFTYPE: pixel formats */
#define RGBFB_NONE      0
#define RGBFB_CLUT      1
#define RGBFB_R8G8B8    2
#define RGBFB_B8G8R8    3
#define RGBFB_R5G6B5PC  4
#define RGBFB_R5G5B5PC  5
#define RGBFB_A8R8G8B8  6
#define RGBFB_A8B8G8R8  7
#define RGBFB_R8G8B8A8  8
#define RGBFB_B8G8R8A8  9
#define RGBFB_R5G6B5    10
#define RGBFB_R5G5B5    11
#define RGBFB_B5G6R5PC  12
#define RGBFB_B5G5R5PC  13

/* p96GetModeIDAttr() attributes */
#define P96IDA_WIDTH          0
#define P96IDA_HEIGHT         1
#define P96IDA_DEPTH          2
#define P96IDA_BYTESPERPIXEL  3
#define P96IDA_BITSPERPIXEL   4
#define P96IDA_RGBFORMAT      5
#define P96IDA_ISP96          6
#define P96IDA_BOARDNUMBER    7
#define P96IDA_STDBYTESPERROW 8
#define P96IDA_BOARDNAME      9

/* p96GetBitMapAttr() attributes */
#define P96BMA_WIDTH          0
#define P96BMA_HEIGHT         1
#define P96BMA_DEPTH          2
#define P96BMA_MEMORY         3
#define P96BMA_BYTESPERROW    4
#define P96BMA_BYTESPERPIXEL  5
#define P96BMA_BITSPERPIXEL   6
#define P96BMA_RGBFORMAT      7
#define P96BMA_ISP96          8
#define P96BMA_ISONBOARD      9

/* p96LockBitMap() fills one of these */
struct RenderInfo {
	APTR  Memory;
	WORD  BytesPerRow;
	WORD  pad;
	ULONG RGBFormat;
};

/* -42 p96GetBitMapAttr(BitMap, Attribute)(a0, d0) */
static inline ULONG p96GetBitMapAttr(struct BitMap *bm, ULONG attr)
{
	register struct Library *_b __asm("a6") = P96Base_rtg;
	register struct BitMap *_a0 __asm("a0") = bm;
	register ULONG _d0 __asm("d0") = attr;
	__asm volatile ("jsr %%a6@(-42:W)" : "+d"(_d0), "+a"(_a0) : "a"(_b) : "d1", "a1", "fp0", "fp1", "cc", "memory");
	return _d0;
}

/* -48 p96LockBitMap(BitMap, Buffer, Size)(a0, a1, d0) -> lock (0 = failed) */
static inline LONG p96LockBitMap(struct BitMap *bm, UBYTE *buf, ULONG size)
{
	register struct Library *_b __asm("a6") = P96Base_rtg;
	register struct BitMap *_a0 __asm("a0") = bm;
	register UBYTE *_a1 __asm("a1") = buf;
	register ULONG _d0 __asm("d0") = size;
	__asm volatile ("jsr %%a6@(-48:W)" : "+d"(_d0), "+a"(_a0), "+a"(_a1) : "a"(_b) : "d1", "fp0", "fp1", "cc", "memory");
	return (LONG)_d0;
}

/* -54 p96UnlockBitMap(BitMap, Lock)(a0, d0) */
static inline void p96UnlockBitMap(struct BitMap *bm, LONG lock)
{
	register struct Library *_b __asm("a6") = P96Base_rtg;
	register struct BitMap *_a0 __asm("a0") = bm;
	register LONG _d0 __asm("d0") = lock;
	__asm volatile ("jsr %%a6@(-54:W)" : "+d"(_d0), "+a"(_a0) : "a"(_b) : "d1", "a1", "fp0", "fp1", "cc", "memory");
}

/* -84 p96GetModeIDAttr(Mode, Attribute)(d0, d1) */
static inline ULONG p96GetModeIDAttr(ULONG id, ULONG attr)
{
	register struct Library *_b __asm("a6") = P96Base_rtg;
	register ULONG _d0 __asm("d0") = id;
	register ULONG _d1 __asm("d1") = attr;
	__asm volatile ("jsr %%a6@(-84:W)" : "+d"(_d0), "+d"(_d1) : "a"(_b) : "a0", "a1", "fp0", "fp1", "cc", "memory");
	return _d0;
}

/* -102 p96WritePixelArray(ri, SrcX, SrcY, rp, DestX, DestY, SizeX, SizeY)
 *      (a0, d0, d1, a1, d2, d3, d4, d5) */
static inline void p96WritePixelArray(struct RenderInfo *ri, UWORD sx, UWORD sy, struct RastPort *rp,
                                      UWORD dx, UWORD dy, UWORD w, UWORD h)
{
	register struct Library *_b __asm("a6") = P96Base_rtg;
	register struct RenderInfo *_a0 __asm("a0") = ri;
	register struct RastPort *_a1 __asm("a1") = rp;
	register ULONG _d0 __asm("d0") = sx;
	register ULONG _d1 __asm("d1") = sy;
	register ULONG _d2 __asm("d2") = dx;
	register ULONG _d3 __asm("d3") = dy;
	register ULONG _d4 __asm("d4") = w;
	register ULONG _d5 __asm("d5") = h;
	__asm volatile ("jsr %%a6@(-102:W)" : "+d"(_d0), "+d"(_d1), "+a"(_a0), "+a"(_a1)
	                : "a"(_b), "d"(_d2), "d"(_d3), "d"(_d4), "d"(_d5) : "fp0", "fp1", "cc", "memory");
}

#endif
