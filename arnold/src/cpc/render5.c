#include <stdio.h>
/* 
 *  Arnold emulator (c) Copyright, Kevin Thacker 1995-2001
 *  
 *  This file is part of the Arnold emulator source code distribution.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
/* Rendering Functions */

#include "cpcglob.h"
#include "cpc.h"
#include "render.h"
#include "host.h"
#include "packedimage.h"

#define SHOW_SPEED

#ifdef SHOW_SPEED
#include "headers.h"
#endif

#define LESS_MULTS 

/*
#define CRTC_PIXELS_PER_CLOCK	16

static unsigned char CRTC_FirstVisibleClock;
static unsigned char CRTC_LastVisibleClock;

static void	CRTC_SetVisibleClocks
*/




/* if true, then memory has been setup to be filled with CPC graphics data, otherwise
no, so don't attempt to render anything */
static BOOL Renderer_Active = FALSE;
static int     BytesPerPixel;
int ScanLines = 0;
int FillScanLines = 0;
/* Amiga port: set by a host that wants to know which surface lines changed in
 * Render_DumpScreen4 (NULL: copy every line, as before). */
void (*Render_LineChangedHook)(int SurfaceLine) = NULL;
/* Visible window (see InitialiseRender): bytes within a row, and rows. Until
 * set, everything is drawn. */
int Render_VisX0 = 0, Render_VisX1 = 0x7fffffff, Render_CellBytes = 8;
static int Render_VisTop = 0, Render_VisLines = 0x7fffffff;
int PIXEL_STEP;   /* Amiga port: not static */
int PIXEL_STEP_SHIFT;
static int Render_CPCRenderHeight, Render_CPCRenderWidth;
static int Render_CPCXOffset, Render_CPCYOffset;


static void	Render_SetColourNULL(const RGBCOLOUR *pColour,/*int Red, 
int Green, int Blue,*/ int Index);
static void Render_PutDataWordNULL(int, unsigned, int);
static void Render_PutSyncNULL(int, int);
static void Render_PutBorderNULL(int, int);
static void Render_PutDataWordPLUSNULL(int HorizontalCount,unsigned GraphicsData, int Line, unsigned Mask, int *pPixels);

static void (*pRender_DumpScreen)(void) = NULL;
static void (*pRender_SetColour)(const RGBCOLOUR *pColour,/*int, int, 
int,*/ int)=Render_SetColourNULL;
static void (*pRender_PutDataWord)(int, unsigned, int)=Render_PutDataWordNULL;
static void (*pRender_PutSync)(int, int)=Render_PutSyncNULL;
static void (*pRender_PutBorder)(int, int)=Render_PutBorderNULL;
static void (*pRender_PutDataWordPLUS)(int,unsigned, int, unsigned, int *) = Render_PutDataWordPLUSNULL;

/* start of screen buffer */
static unsigned char    *pScreenBase = NULL;
/* number of bytes in screen buffer width */
static unsigned    ScreenPitch = 0;
static unsigned ScreenHeight = 0;
unsigned char *pScreenLine;   /* Amiga port: not static, crtc.c checks visibility inline */

/* current graphics format */
static GRAPHICS_FORMAT  CurrentGraphicsFormat;

static int Render_ScreenXOffset, Render_ScreenYOffset;



/* colours not converted into destination format - this
is used when conversion between different colour formats is required
or conversion from a TrueColour to a paletted mode */
static PALETTE_ENTRY_RGB888    UnConvertedColourTable[32];

/* **** RGB stuff **** */

/* each element contains colours packed into destination image format */
unsigned    ConvertedColourTable[32];   /* Amiga port: not static (crtc.c inline border) */

/* TrueColour RGB version of set colour */
static void Render_TrueColourRGB_SetColour(const RGBCOLOUR *pColour, 
/*int, int, int,*/int);
static void Render_TrueColourRGB_Setup(void);

/* **** PALETTE stuff **** */

/* this array is indexed with the pen index to display on this
screen, this then gives the index within the palette below for
the actual colour */
static signed int PaletteRemap[32];
extern unsigned long Render_RemapStamp;   /* Amiga port: defined with the pixel cache below */


/* colours in palette */

/* two palettes, one is visible while the other is being built */
static PALETTE_ENTRY    CPCPalette[256];

/* pointer to the visible palette */
/* palette we build up */

/* Paletted version of set colour */
static void Render_Paletted_SetColour(const RGBCOLOUR *pColour, 
/*int,int,int,*/int);
static void Render_Paletted_Setup(void);



/*--------------------------------------------------------------------------*/
/* TrueColour RGB Stuff */

#define Render_TrueColourRGB_WriteColourToScreenBuffer(pScreen,ColourIndex) \
        ((unsigned *)pScreen)[0] = ConvertedColourTable[ColourIndex]


/* given a R,G,B in 8:8:8 format, these tables will give the corresponding
R,G,B in X:Y:Z format */
static unsigned RedConversionTable[256];
static unsigned GreenConversionTable[256];
static unsigned BlueConversionTable[256];


static void    Render_TrueColourRGB_BuildConversionTable(unsigned
*pTable, GRAPHICS_ELEMENT_FORMAT *pElement)
{
    int i;
    
    /* max value colour element can take */
    unsigned MaxValue = 0x0ffffffff>>(32-pElement->BPP);

    /* this fraction is added each time through the loop. The upper
    16-bits hold the integer part and the lower 16-bits hold the fractional 
    part. The following calculation works as long as the element has less
    than 16 bits for it's representation. (16-bit R, 16-bit G, 16-bit B is massive!!!) */
    unsigned AddFraction = ((MaxValue+1)<<16)>>8;

    unsigned CurrentValue = 0;

    for (i=0; i<256; i++)
    {
            /* store colour in packed colour format */
            pTable[i] = ((CurrentValue>>16)<<pElement->Shift);

            CurrentValue += AddFraction;
    }
}

static void    Render_TrueColourRGB_BuildConversionTables(GRAPHICS_FORMAT
*pFormat)
{
        /* build red conversion table */
        Render_TrueColourRGB_BuildConversionTable(RedConversionTable,
&pFormat->Red);
        /* build green conversion table */
        Render_TrueColourRGB_BuildConversionTable(GreenConversionTable,
&pFormat->Green);
        /* build blue conversion table */
        Render_TrueColourRGB_BuildConversionTable(BlueConversionTable,
&pFormat->Blue);
}

static void Render_TrueColourRGB_ConvertColoursToNewFormat(GRAPHICS_FORMAT
*pDestFormat)
{
    int i;

    for (i=0; i<32; i++)
    {
            unsigned r, g, b;
			unsigned PackedColourData;

			r = UnConvertedColourTable[i].RGB.SeperateElements.u.element.Red;
			g = UnConvertedColourTable[i].RGB.SeperateElements.u.element.Green;
			b = UnConvertedColourTable[i].RGB.SeperateElements.u.element.Blue;

            /* store R,G,B colour into destination format */
            PackedColourData = PackRGBIntoDestinationImageFormat(r, g, b, pDestFormat);
            
            /* write packed R,G,B in dest format */
            ConvertedColourTable[i] = PackedColourData;
    }
}

static void Render_TrueColourRGB_Setup(void)
{
   Render_TrueColourRGB_BuildConversionTables(&CurrentGraphicsFormat);

   Render_TrueColourRGB_ConvertColoursToNewFormat(&CurrentGraphicsFormat);

	pRender_SetColour = Render_TrueColourRGB_SetColour;
	pRender_PutSync = Render_TrueColourRGB_PutSync;
	pRender_PutBorder = Render_TrueColourRGB_PutBorder;
	pRender_PutDataWord = Render_TrueColourRGB_PutDataWord;
	pRender_PutDataWordPLUS = Render_TrueColourRGB_PutDataWordPLUS;

	CRTC_SetTrueColourRender(TRUE);

}

void	Render_TrueColourRGB_SetColour(const RGBCOLOUR *pColour,/*int Red, 
int Green, int Blue,*/ int
Index)
{
	/* convert R,G,B into format of screen pixel */
        ConvertedColourTable[Index] = (RedConversionTable[pColour->u.element.Red & 0x0ff] | 
			GreenConversionTable[pColour->u.element.Green & 0x0ff] | 
			BlueConversionTable[pColour->u.element.Blue & 0x0ff]);
}

/*------------------------------------------------------------------------*/
/* PALETTED STUFF */

#define Render_Paletted_WriteColourToScreenBuffer(pScreen, ColourIndex) \
        ((unsigned char *)pScreen)[0] = PaletteRemap[ColourIndex]

int Render_PaletteIndexBlack = 0;   /* Amiga port: not static (crtc.c inline sync) */
int Render_PaletteIndexBorder = 0;  /* border fix: palette index of pen 16 (see Render_Paletted_PutBorder) */


/* find a colour match */
int     Palette_MatchColour(int Red, int Green, int Blue)
{
        int i;

        for (i=0; i<256; i++)
        {
                if (CPCPalette[i].Flags & PALETTE_ENTRY_USED)
                {
                        if (!(CPCPalette[i].Flags & PALETTE_ENTRY_DO_NOT_MATCH))
                        {
                                if ((CPCPalette[i].Red == Red) && (CPCPalette[i].Green == Green) && (CPCPalette[i].Blue == Blue))
                                {
                                        return i;
                                }
                        }
                }
        }

        return -1;
}


/* find a free palette entry */
int     Palette_StoreColour(int Red, int Green, int Blue)
{
        int i;

        for (i=0; i<256; i++)
        {
                /* is this entry free? */
                if (!(CPCPalette[i].Flags & PALETTE_ENTRY_USED))
                {
                        /* free palette entry */

                        CPCPalette[i].Red = (unsigned char)Red;
                        CPCPalette[i].Green = (unsigned char)Green;
                        CPCPalette[i].Blue = (unsigned char)Blue;

                        /* mark it as used */
                        CPCPalette[i].Flags |= PALETTE_ENTRY_USED;
                        

						return i;
                }
        }

        /* if we get to here, no palette entries were free. */

        return -1;
}



/* current method is a bit slow, tries to keep all pens that
use the same colour pointing to same palette index, and drops
any colours that don't make it into the palette */
int     GetIndexInPalette(int PenIndex, int Red, int Green, int Blue)
{
    int PaletteIndex;
    int CurrentPaletteIndex;

    CurrentPaletteIndex = PaletteRemap[PenIndex];

	if (CurrentPaletteIndex==-1)
	{
		/* find a match */
		PaletteIndex = Palette_MatchColour(Red, Green, Blue);

		if (PaletteIndex==-1)
		{
			/* no match. Store colour in a free entry */
			PaletteIndex = Palette_StoreColour(Red, Green, Blue);
		}

		return PaletteIndex;
	}
	else
	{
		/* colour changed? */
		if ((CPCPalette[CurrentPaletteIndex].Red == Red) &&
			(CPCPalette[CurrentPaletteIndex].Green == Green) &&
			(CPCPalette[CurrentPaletteIndex].Blue == Blue))
		{
			/* same colour as before */
			PaletteIndex = CurrentPaletteIndex;
		}
		else
		{
			/* yes it's changed */

			/* find a match against existing colours in palette */
			PaletteIndex = Palette_MatchColour(Red, Green, Blue);

			if (PaletteIndex==-1)
			{
				/* no match. Store colour in a free entry */
				PaletteIndex = Palette_StoreColour(Red, Green, Blue);
			
				if (PaletteIndex==-1)
				{
					/* oh dear, can't store it, colour has just
					been dropped from palette */

					PaletteIndex = CurrentPaletteIndex;
				}
			}
		}
	}

    return PaletteIndex;
}


static void	Render_Paletted_SetColour(const RGBCOLOUR *pColour,/*int 
Red, int Green, int Blue,*/
int Index)
{
	int PaletteIndex;

	PaletteIndex = GetIndexInPalette(Index, pColour->u.element.Red, pColour->u.element.Green, pColour->u.element.Blue);
        
	if (PaletteIndex!=-1)
	{
		/* set mapping */
		PaletteRemap[Index] = PaletteIndex;
		Render_RemapStamp++;   /* Amiga port: cached remapped pixels are stale */
		if (Index == 16) Render_PaletteIndexBorder = PaletteIndex;
	}
}

static void	Render_Paletted_Setup(void)
{
	pRender_SetColour = Render_Paletted_SetColour;

	pRender_PutSync = Render_Paletted_PutSync;
	pRender_PutBorder = Render_Paletted_PutBorder;
	pRender_PutDataWord = Render_Paletted_PutDataWord;
	pRender_PutDataWordPLUS = Render_Paletted_PutDataWordPLUS;

	CRTC_SetTrueColourRender(FALSE);

}


static void Render_Paletted_ClearRemapping(void)
{
	int i;
	Render_PaletteIndexBorder = Render_PaletteIndexBlack;   /* border fix */

	/* clear all re-map stuff */
	for (i=0; i<32; i++)
	{
		PaletteRemap[i] = -1;
		Render_RemapStamp++;   /* Amiga port: cached remapped pixels are stale */
	}
}

/* Amiga port (18 Oct 2026): palette entry 0 is kept black for good. The
 * paletted renderer draws blanking with Render_PaletteIndexBlack (0), and on
 * an Amiga entry 0 is also colour register 0, which fills everything outside
 * the picture. It used to go to whichever colour was stored first, so after
 * the palette was freed (Power ON, or freeing when full) a game's first colour
 * filled the screen surround (red bars around Ghosts'n Goblins on the A1200).
 * NO_BLACK0_FIX: entry 0 is an ordinary entry, as before. */
static void Render_ReserveBlack(void)
{
#ifndef NO_BLACK0_FIX
	CPCPalette[0].Red = 0; CPCPalette[0].Green = 0; CPCPalette[0].Blue = 0;
	CPCPalette[0].Flags = PALETTE_ENTRY_USED | PALETTE_ENTRY_DO_NOT_REMOVE;
#endif
}

void    Palette_Initialise(void)
{
	Render_Paletted_ClearRemapping();
	Render_ReserveBlack();
}

/* mark a palette entry as useable by host system, the palette
stuff will not then attempt to touch this colour */
void	Render_MarkPaletteEntryForHostUse(int Index)
{
	if ((Index<0) || (Index>255))
		return;

    CPCPalette[Index].Red = (unsigned char)Index;
    CPCPalette[Index].Green = 0;
    CPCPalette[Index].Blue = 0;
    CPCPalette[Index].Flags = PALETTE_ENTRY_USED | PALETTE_ENTRY_DO_NOT_MATCH | PALETTE_ENTRY_DO_NOT_REMOVE;
}    

/* reset the palette. This can be done every VBL, so we can have different colours each
 VBL. */
void    Palette_Reset(void)
{
    int i;

    for (i=0; i<256; i++)
    {
        if (!(CPCPalette[i].Flags & PALETTE_ENTRY_DO_NOT_REMOVE))
        {
                CPCPalette[i].Flags = 0;
        }
    }
	Render_ReserveBlack();

	for (i=0; i<32; i++)
	{
		PaletteRemap[i] = -1;
		Render_RemapStamp++;   /* Amiga port: cached remapped pixels are stale */
	}

	CPC_UpdateColours();
}


void	Palette_Set(void)
{
	int i;

	for (i=0; i<256; i++)
	{
		if (CPCPalette[i].Flags & PALETTE_ENTRY_USED)
		{
			if (!(CPCPalette[i].Flags & PALETTE_ENTRY_DO_NOT_MATCH))
			{
				unsigned char Red, Green, Blue;

				Red = CPCPalette[i].Red;
				Green = CPCPalette[i].Green;
				Blue = CPCPalette[i].Blue;

				/* set palette entry in host */
				Host_SetPaletteEntry(i, (unsigned char)Red, (unsigned char)Green, (unsigned char)Blue);
			}
		}
	}
}




/*-----------------------------------------------------------------------*/


#ifdef LESS_MULTS
#define Render_CalcRenderAddress(HorizontalCount, Line)	\
	char *pScreen = (char *)pScreenLine
#else
#define Render_CalcRenderAddress(HorizontalCount,Line)		\
    int XPixel = (HorizontalCount<<(1+3))>>PIXEL_STEP_SHIFT;		\
    int     ScreenOffset = (Line*ScreenPitch) + XPixel*BytesPerPixel; \
	char	*pScreen = (char *)((unsigned long)pScreenBase + (unsigned long)ScreenOffset)
#endif

#ifdef LESS_MULTS
#define Render_UpdateRenderAddress			\
         pScreenLine = (unsigned char *)pScreen
#else
#define Render_UpdateRenderAddress
#endif

void    Render_Finish(void)
{
        if (pScreenBase!=NULL)
        {
                free(pScreenBase);
        }
}

void	Render_SetColourNULL(const RGBCOLOUR *pColour,/*int Red, int 
Green, int Blue,*/ int Index)
{
}

static void Render_PutDataWordNULL(int HorizontalCount, unsigned GraphicsWord, int Line)
{
}

static void Render_PutSyncNULL(int HorizontalCount, int Line)
{
}

static void Render_PutBorderNULL(int HorizontalCount, int Line)
{
}

static void Render_PutDataWordPLUSNULL(int HorizontalCount,unsigned  GraphicsData, int Line, unsigned Mask, int *pPixels)
{
}



/* set pen index specified by Index to Red, Green and Blue specified */
void    Render_SetColour(const RGBCOLOUR *pColour,/*int Red,int Green,int 
Blue,*/ int Index)
{
	/* store chosen colour - in conversion table */
	UnConvertedColourTable[Index].RGB.SeperateElements.u.element.Red = pColour->u.element.Red;
	UnConvertedColourTable[Index].RGB.SeperateElements.u.element.Green = pColour->u.element.Green;
	UnConvertedColourTable[Index].RGB.SeperateElements.u.element.Blue = pColour->u.element.Blue;
	
	/* update palette (or encode into screen pixel format)
	depending on display */
	pRender_SetColour(pColour,/*Red,Green,Blue,*/Index);
}

void    Render_PutDataWord(int HorizontalCount,unsigned GraphicsData, int Line)
{
	pRender_PutDataWord(HorizontalCount, GraphicsData, Line);
}

void	Render_PutSync(int HorizontalCount, int Line)
{
	pRender_PutSync(HorizontalCount, Line);
}

void    Render_PutBorder(int HorizontalCount, int Line)
{
	pRender_PutBorder(HorizontalCount, Line);
}

/* faster, but may not be accurate enough */
void    Render_PutDataWordPLUSMaskWithPixels(int HorizontalCount,unsigned GraphicsData, int Line, unsigned Mask, int *pPixels)
{
	pRender_PutDataWordPLUS(HorizontalCount,GraphicsData, Line, Mask, pPixels);
}


unsigned *pPackedPixels;
PIXEL_DATA *pPixelData;
int CurrentMode;
int CurrentModeMask;
int CurrentModeShift;
int ModeHorizontalPixelScroll;
int CurrentTopShift;
static int HorizontalPixelScroll = 0;

/* Amiga port (15 Oct 2026): Plus horizontal soft scroll in the paletted
 * renderer. The scroll delays the pixel stream by whole pixels of the current
 * mode (ModeHorizontalPixelScroll = dots >> mode shift). The paletted path did
 * a plain bit shift of the two graphics bytes, which is right only in mode 2:
 * in modes 0 and 1 a pixel's bits are interleaved across the byte, so the
 * shift mixed bits of neighbouring pixels (outlines right, colours scrambled:
 * Sonic GX throughout, Ghosts'n Goblins at times). This is the per-pixel shift
 * Arnold's true-colour renderer already uses. NO_HSCROLL_FIX: the old shift. */
/* Sonic speed (20 Oct 2026): the per-pixel loop in Render_ScrollGraphicsData
 * moves each input bit to a fixed place (masks and shifts distribute over OR),
 * so its result is the OR of what each input byte gives alone: four lookups in
 * a table built on first use per mode (0, 1) and scroll (1-7). Only the low 16
 * bits are kept; all callers use only those (the two graphics bytes). Checked
 * against the loop on the PC (build/sonic-speed/scroll_test.c). NULL when the
 * table doesn't apply (mode 2, no scroll). crtc.c's run loop uses it directly.
 * NO_SCROLL_TABLE: the loop only. */
const unsigned short *Render_ScrollTable(void)
{
#ifndef NO_SCROLL_TABLE
	static unsigned short Tab[2][8][4 * 256];
	static int TabMask[2][8], TabTop[2][8];
	const int m = CurrentMode, sc = ModeHorizontalPixelScroll;
	unsigned short *t;
	if (m < 0 || m > 1 || sc <= 0 || sc > 7) return NULL;
	t = Tab[m][sc];
	if (TabMask[m][sc] != CurrentModeMask || TabTop[m][sc] != CurrentTopShift || !TabMask[m][sc])
	{
		int k, b, j;
		for (k = 0; k < 4; k++)
			for (b = 0; b < 256; b++)
			{
				unsigned int d = (unsigned int)b << (8 * k);
				for (j = 0; j < sc; j++)
					d = ((d & (unsigned int)CurrentModeMask) >> 1) | ((d & ~(unsigned int)CurrentModeMask) >> CurrentTopShift);
				t[k * 256 + b] = (unsigned short)d;
			}
		TabMask[m][sc] = CurrentModeMask; TabTop[m][sc] = CurrentTopShift;
	}
	return t;
#else
	return NULL;
#endif
}

unsigned int Render_ScrollGraphicsData(unsigned int Data)
{
	int i;
#ifdef NO_HSCROLL_FIX
	return Data >> ModeHorizontalPixelScroll;
#else
	if (CurrentMode == 2)
		return Data >> ModeHorizontalPixelScroll;
#ifndef NO_SCROLL_TABLE
	{
		const unsigned short *t = Render_ScrollTable();
		if (t) return (unsigned int)t[Data & 0x0ff] | t[256 + ((Data >> 8) & 0x0ff)] | t[512 + ((Data >> 16) & 0x0ff)] | t[768 + (Data >> 24)];
	}
#endif
	for (i = 0; i < ModeHorizontalPixelScroll; i++)
		Data = ((Data & (unsigned int)CurrentModeMask) >> 1) | ((Data & ~(unsigned int)CurrentModeMask) >> CurrentTopShift);
	return Data;
#endif
}

/* Amiga port: B fast path. For each mode and byte value, the screen bytes a
 * graphics byte becomes (pens through PaletteRemap), built on first use and
 * rebuilt after any PaletteRemap or PIXEL_STEP change (Render_RemapStamp).
 * Lets a cell without sprites be written as 32-bit stores. */
unsigned long Render_RemapStamp = 1;
static RENDER_PIXCACHE Render_PixCache[4][256];
static PIXEL_DATA *Render_PixCacheKey[4];
RENDER_PIXCACHE *pPixCache = Render_PixCache[0];

static void Render_SelectPixCache(void)
{
	int slot = CurrentMode & 3;
	if (Render_PixCacheKey[slot] != pPixelData)
	{
		int i;
		Render_PixCacheKey[slot] = pPixelData;
		for (i = 0; i < 256; i++) Render_PixCache[slot][i].Stamp = 0;
	}
	pPixCache = Render_PixCache[slot];
}

const RENDER_PIXCACHE *Render_CachedPixels(unsigned Byte)
{
	RENDER_PIXCACHE *e = &pPixCache[Byte];
	if (e->Stamp != Render_RemapStamp)
	{
		const PIXEL_DATA *pd = &pPixelData[Byte];
		int i, k = 0;
		for (i = 0; i < 8; i += PIXEL_STEP)
			e->u.b[k++] = (unsigned char)PaletteRemap[pd->Pixel[i]];
		e->Stamp = Render_RemapStamp;
	}
	return e;
}

/* one graphics byte: 4 screen bytes (lores) or 8 (PIXEL_STEP 1) */
#define Render_FastPutByte(pScreen, Byte)										{																			const RENDER_PIXCACHE *e_ = Render_CachedPixels(Byte);					((unsigned int *)(pScreen))[0] = e_->u.l[0];							if (PIXEL_STEP == 1) ((unsigned int *)(pScreen))[1] = e_->u.l[1]; 		pScreen += 8 / PIXEL_STEP;											}

void    Render_SetPixelTranslation(int ModeIndex)
{
        CurrentMode = ModeIndex;

        switch (CurrentMode)
        {
                case 0:
                        CurrentModeMask = 0x0aaaaaaaa;
                        CurrentModeShift = 2;
                        CurrentTopShift = 7;
						break;
                case 1:
                        CurrentModeMask = 0x0eeeeeeee;  
                        CurrentModeShift = 1;
                        CurrentTopShift = 5;
                        break;
                case 2:
                        CurrentModeMask = 0x07f7f7f7f;
                        CurrentModeShift = 0;
                        CurrentTopShift = 1;
						break;
        }
#ifndef NO_HSCROLL_FIX
        ModeHorizontalPixelScroll = HorizontalPixelScroll >> CurrentModeShift;
#endif

        pPixelData = CPC_GetModePixelData(ModeIndex);
		pPackedPixels = CPC_GetModePackedPixelData(ModeIndex);
		Render_SelectPixCache();   /* Amiga port */

}

#define GENERIC

#ifdef GENERIC
void    Render_TrueColourRGB_PutDataWord(int HorizontalCount,unsigned GraphicsData, int Line)
{
    int i;
    PIXEL_DATA *pThisPixelData;

	Render_CalcRenderAddress(HorizontalCount,Line);

    pThisPixelData = &pPixelData[(unsigned)((GraphicsData>>8) & 0x0ff)];

    for (i=0; i<8; i+=PIXEL_STEP)
    {
        int     Pixel;

        Pixel = pThisPixelData->Pixel[i];
        Render_TrueColourRGB_WriteColourToScreenBuffer(pScreen, Pixel);
        pScreen+=BytesPerPixel;
    }

    pThisPixelData = &pPixelData[(unsigned)(GraphicsData & 0x0ff)];

    for (i=0; i<8; i+=PIXEL_STEP)
    {
        int Pixel;

        Pixel = pThisPixelData->Pixel[i];
        Render_TrueColourRGB_WriteColourToScreenBuffer(pScreen, Pixel);
        pScreen+=BytesPerPixel;
    }

	Render_UpdateRenderAddress;
}
#else
void    Render_TrueColourRGB_PutDataWord(int HorizontalCount,unsigned GraphicsData, int Line)
{
	int ScreenPixel;
	unsigned PackedPixels;
	unsigned Pixel;
	Render_CalcRenderAddress(HorizontalCount,Line);


	if (PIXEL_STEP == 1) /* Fixed! RFB */
	{
		PackedPixels = pPackedPixels[(unsigned)((GraphicsData>>8) & 0x0ff)];
		
		 Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>4;
		ScreenPixel = ConvertedColourTable[Pixel];
		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>4;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[0] = ScreenPixel;

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>4;
		ScreenPixel = ConvertedColourTable[Pixel];
		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>4;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[1] = ScreenPixel;

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>4;
		ScreenPixel = ConvertedColourTable[Pixel];
		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>4;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[2] = ScreenPixel;

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>4;
		ScreenPixel = ConvertedColourTable[Pixel];
		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>4;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[3] = ScreenPixel;
		
		pScreen += (4 * 4);
		
		PackedPixels = pPackedPixels[(unsigned)(GraphicsData & 0x0ff)];
		
		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>4;
		ScreenPixel = ConvertedColourTable[Pixel];
		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>4;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[0] = ScreenPixel;

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>4;
		ScreenPixel = ConvertedColourTable[Pixel];
		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>4;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[1] = ScreenPixel;

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>4;
		ScreenPixel = ConvertedColourTable[Pixel];
		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>4;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[2] = ScreenPixel;

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>4;
		ScreenPixel = ConvertedColourTable[Pixel];
		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>4;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[3] = ScreenPixel;
		
		pScreen += (4 * 4);
	}
	else
    {
		

	 PackedPixels = pPackedPixels[(unsigned)((GraphicsData>>8) & 0x0ff)];


#ifndef CPC_LSB_FIRST
	PackedPixels = (((PackedPixels & 0x0000FF00) >> 8) |
					((PackedPixels & 0x000000FF) << 8) |
					((PackedPixels & 0xFF000000) >> 8) |
					((PackedPixels & 0x00FF0000) << 8));
#endif	

        Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>8;
		ScreenPixel = ConvertedColourTable[Pixel];
		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>8;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[0] = ScreenPixel;

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>8;
		ScreenPixel = ConvertedColourTable[Pixel];
		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>8;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[1] = ScreenPixel;

   		PackedPixels = pPackedPixels[(unsigned)(GraphicsData & 0x0ff)];

#ifndef CPC_LSB_FIRST
	PackedPixels = (((PackedPixels & 0x0000FF00) >> 8) |
					((PackedPixels & 0x000000FF) << 8) |
					((PackedPixels & 0xFF000000) >> 8) |
					((PackedPixels & 0x00FF0000) << 8));
#endif	

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>8;
		ScreenPixel = ConvertedColourTable[Pixel];
		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>8;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[2] = ScreenPixel;

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>8;
		ScreenPixel = ConvertedColourTable[Pixel];
		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>8;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[3] = ScreenPixel;

		pScreen+=(4*4);
    }

	Render_UpdateRenderAddress;
}
#endif

#define CPC_WRITE_PIXEL_INC(ci, dest,increment)	\
		((unsigned *)dest)[0] = ci;	\
		dest = &dest[increment]

#ifdef GENERIC
void    Render_TrueColourRGB_PutSync(int HorizontalCount, int Line)
{
    int i;

    Render_CalcRenderAddress(HorizontalCount,Line);

    for (i=0; i<16; i+=PIXEL_STEP)
    {
       ((unsigned *)pScreen)[0] = 0x0;
       pScreen+=BytesPerPixel;
    }

	Render_UpdateRenderAddress;
}
#else
void    Render_TrueColourRGB_PutSync(int HorizontalCount, int Line)
{

    Render_CalcRenderAddress(HorizontalCount,Line);


	((unsigned *)pScreen)[0] = 0x0;
	((unsigned *)pScreen)[1] = 0x0;
	((unsigned *)pScreen)[2] = 0x0;
	((unsigned *)pScreen)[3] = 0x0;
	pScreen+=(4*4);
	Render_UpdateRenderAddress;
}
#endif

#ifdef GENERIC
void    Render_TrueColourRGB_PutBorder(int HorizontalCount, int Line)
{
    int i;
	int BorderColour = ConvertedColourTable[16];

    Render_CalcRenderAddress(HorizontalCount,Line);

    for (i=0; i<16; i+=PIXEL_STEP)
    {
       ((unsigned *)pScreen)[0] = BorderColour;
       pScreen+=BytesPerPixel;
    }

	Render_UpdateRenderAddress;
}
#else
void    Render_TrueColourRGB_PutBorder(int HorizontalCount, int Line)
{
	int BorderColour;

	Render_CalcRenderAddress(HorizontalCount, Line);
	
	BorderColour = ConvertedColourTable[16];
	BorderColour = BorderColour | (BorderColour<<16);

	((unsigned *)pScreen)[0] = BorderColour;
	((unsigned *)pScreen)[1] = BorderColour;
	((unsigned *)pScreen)[2] = BorderColour;
	((unsigned *)pScreen)[3] = BorderColour;
	pScreen+=(4*4);

	Render_UpdateRenderAddress;
}
#endif

void    Render_Paletted_PutDataWord(int HorizontalCount,unsigned GraphicsData, int Line)
{
    int i;
    PIXEL_DATA *pThisPixelData;

	Render_CalcRenderAddress(HorizontalCount,Line);

#ifndef NO_B
	/* Amiga port: B fast path (cached, remapped, 32-bit stores) */
	(void)i; (void)pThisPixelData;
	Render_FastPutByte(pScreen, (GraphicsData>>8) & 0x0ff);
	Render_FastPutByte(pScreen, GraphicsData & 0x0ff);
#else
    pThisPixelData = &pPixelData[(unsigned)((GraphicsData>>8) & 0x0ff)];
    for (i=0; i<8; i+=PIXEL_STEP) { Render_Paletted_WriteColourToScreenBuffer(pScreen, pThisPixelData->Pixel[i]); pScreen+=BytesPerPixel; }
    pThisPixelData = &pPixelData[(unsigned)(GraphicsData & 0x0ff)];
    for (i=0; i<8; i+=PIXEL_STEP) { Render_Paletted_WriteColourToScreenBuffer(pScreen, pThisPixelData->Pixel[i]); pScreen+=BytesPerPixel; }
#endif

	Render_UpdateRenderAddress;
}


void    Render_Paletted_PutSync(int HorizontalCount, int Line)
{
    Render_CalcRenderAddress(HorizontalCount,Line);

#ifdef NO_B
	{ int i; for (i=0; i<16; i+=PIXEL_STEP) { ((unsigned char *)pScreen)[0] = Render_PaletteIndexBlack; pScreen+=BytesPerPixel; } }
#else
	/* Amiga port: same bytes as 16/PIXEL_STEP byte stores, as longs */
	{
		unsigned int fill = (unsigned int)(unsigned char)Render_PaletteIndexBlack * 0x01010101U;
		unsigned int *d = (unsigned int *)pScreen;
		d[0] = fill; d[1] = fill;
		if (PIXEL_STEP == 1) { d[2] = fill; d[3] = fill; }
		pScreen += 16 / PIXEL_STEP;
	}
#endif

	Render_UpdateRenderAddress;
}

void    Render_Paletted_PutBorder(int HorizontalCount, int Line)
{
	Render_CalcRenderAddress(HorizontalCount, Line);

#ifdef NO_B
	{ int i; for (i=0; i<16; i+=PIXEL_STEP) { Render_TrueColourRGB_WriteColourToScreenBuffer(pScreen, 16); pScreen+=BytesPerPixel; } }
#else
	/* Amiga port: the original writes the 4-byte ConvertedColourTable[16] at
	 * each of the 16/PIXEL_STEP byte positions, so the cell ends up filled with
	 * its first byte and the 3 bytes after the cell get bytes 1-3. Same result
	 * with longs. */
	{
#ifdef NO_BORDER_FIX
		unsigned v = ConvertedColourTable[16];
		const unsigned char *cb = (const unsigned char *)&v;
		unsigned int fill = (unsigned int)cb[0] * 0x01010101U;
#else
		/* Border fix (17 Oct 2026): the border pen's palette index. The
		 * original took ConvertedColourTable[16], which only the true-colour
		 * renderer fills, so the paletted border kept the colour that
		 * palette index 0 happened to hold (BASIC's blue, for ever). */
		unsigned char cb[4];
		unsigned int fill = (unsigned int)(unsigned char)Render_PaletteIndexBorder * 0x01010101U;
		cb[1] = cb[2] = cb[3] = (unsigned char)Render_PaletteIndexBorder;
#endif
		unsigned int *d = (unsigned int *)pScreen;
		d[0] = fill; d[1] = fill;
		if (PIXEL_STEP == 1) { d[2] = fill; d[3] = fill; }
		pScreen += 16 / PIXEL_STEP;
		((unsigned char *)pScreen)[0] = cb[1];
		((unsigned char *)pScreen)[1] = cb[2];
		((unsigned char *)pScreen)[2] = cb[3];
	}
#endif

	Render_UpdateRenderAddress;
}


void    Render_SetHorizontalPixelScroll(int PixelScroll)
{
        HorizontalPixelScroll = PixelScroll;

        ModeHorizontalPixelScroll = HorizontalPixelScroll>>CurrentModeShift;
}


#ifdef GENERIC
/* faster, but may not be accurate enough */
void    Render_TrueColourRGB_PutDataWordPLUS(int HorizontalCount,unsigned GraphicsData, int Line, unsigned Mask, int *pPixels)
{
    int i;

    PIXEL_DATA *pThisPixelData;

	Render_CalcRenderAddress(HorizontalCount,Line);

	GraphicsData = GraphicsData>>(ModeHorizontalPixelScroll>>2);

    {
        pThisPixelData = &pPixelData[(unsigned)(GraphicsData & 0x0ff00)>>8];

        for (i=0; i<8; i+=PIXEL_STEP)
        {
                int     Pixel;

                if ((Mask & (1<<i))!=0)
                {
                        Pixel = pThisPixelData->Pixel[i];
                }
                else
                {
                        Pixel = pPixels[i];
                }

                Render_TrueColourRGB_WriteColourToScreenBuffer(pScreen, Pixel);
                pScreen+=BytesPerPixel;
        }
    }

    {
        pThisPixelData = &pPixelData[(unsigned)(GraphicsData & 0x0ff)];

        for (i=0; i<8; i+=PIXEL_STEP)
        {
                int     Pixel;

                if ((Mask & (1<<(i+8)))!=0)
                {
                        Pixel = pThisPixelData->Pixel[i];
                }
                else
                {
                        Pixel = pPixels[i+8];
                }
                
                Render_TrueColourRGB_WriteColourToScreenBuffer(pScreen, Pixel);
                pScreen+=BytesPerPixel;
        }
    }

	Render_UpdateRenderAddress;
}
#else
void    Render_TrueColourRGB_PutDataWordPLUS(int HorizontalCount,unsigned GraphicsData, int Line, unsigned Mask, int *pPixels)
{
	int i;
    
	Render_CalcRenderAddress(HorizontalCount,Line);

    for (i=0; i<ModeHorizontalPixelScroll; i++)
    {
		GraphicsData = (
						((GraphicsData & CurrentModeMask)>>1) |
						((GraphicsData & (~CurrentModeMask))>>CurrentTopShift)
						);
    }

	if ((Mask & 0x0ffff)==0x0ffff)
	{
		/* all screen pixels */
		unsigned PackedPixels;
		int Pixel;
		unsigned ScreenPixel;

		PackedPixels = pPackedPixels[(unsigned)(GraphicsData & 0x0ff00)>>8];

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>8;
		ScreenPixel = ConvertedColourTable[Pixel];

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>8;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[0] = ScreenPixel;

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>8;
		ScreenPixel = ConvertedColourTable[Pixel];

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>8;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[1] = ScreenPixel;

		PackedPixels = pPackedPixels[(unsigned)(GraphicsData & 0x0ff)];

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>8;
		ScreenPixel = ConvertedColourTable[Pixel];

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>8;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[2] = ScreenPixel;

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>8;
		ScreenPixel = ConvertedColourTable[Pixel];

		Pixel = (PackedPixels & 0x0f);
		PackedPixels = PackedPixels>>8;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[3] = ScreenPixel;


		pScreen+=(4*4);



	}
	else if ((Mask & 0x0ffff)==0x0000)
    {
		unsigned PackedPixels;
		int Pixel;
		unsigned ScreenPixel;

		PackedPixels = pPackedPixels[(unsigned)(GraphicsData & 0x0ff00)>>8];

		{
            Pixel = pPixels[0];
		}

		PackedPixels = PackedPixels>>8;
		ScreenPixel = ConvertedColourTable[Pixel];

		{
            Pixel = pPixels[2];
		}


		PackedPixels = PackedPixels>>8;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[0] = ScreenPixel;

		{
            Pixel = pPixels[4];
		}

		PackedPixels = PackedPixels>>8;
		ScreenPixel = ConvertedColourTable[Pixel];

		{
            Pixel = pPixels[6];
		}


		PackedPixels = PackedPixels>>8;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[1] = ScreenPixel;

		PackedPixels = pPackedPixels[(unsigned)(GraphicsData & 0x0ff)];

		{
            Pixel = pPixels[8];
		}

		PackedPixels = PackedPixels>>8;
		ScreenPixel = ConvertedColourTable[Pixel];

		{
            Pixel = pPixels[10];
		}


		PackedPixels = PackedPixels>>8;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[2] = ScreenPixel;

		{
            Pixel = pPixels[12];
		}

		PackedPixels = PackedPixels>>8;
		ScreenPixel = ConvertedColourTable[Pixel];

		{
            Pixel = pPixels[14];
		}


		PackedPixels = PackedPixels>>8;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[3] = ScreenPixel;


		pScreen+=(4*4);




	}
	else
	{
		unsigned PackedPixels;
		int Pixel;
		unsigned ScreenPixel;

		PackedPixels = pPackedPixels[(unsigned)(GraphicsData & 0x0ff00)>>8];

		if ((Mask & (1<<0))!=0)
        {
			Pixel = (PackedPixels & 0x0f);
		}
		else
		{
            Pixel = pPixels[0];
		}

		PackedPixels = PackedPixels>>8;
		ScreenPixel = ConvertedColourTable[Pixel];

		if ((Mask & (1<<2))!=0)
        {
			Pixel = (PackedPixels & 0x0f);
		}
		else
		{
            Pixel = pPixels[2];
		}


		PackedPixels = PackedPixels>>8;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[0] = ScreenPixel;

		if ((Mask & (1<<4))!=0)
        {
			Pixel = (PackedPixels & 0x0f);
		}
		else
		{
            Pixel = pPixels[4];
		}

		PackedPixels = PackedPixels>>8;
		ScreenPixel = ConvertedColourTable[Pixel];

		if ((Mask & (1<<6))!=0)
        {
			Pixel = (PackedPixels & 0x0f);
		}
		else
		{
            Pixel = pPixels[6];
		}


			PackedPixels = PackedPixels>>8;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[1] = ScreenPixel;

		PackedPixels = pPackedPixels[(unsigned)(GraphicsData & 0x0ff)];

		if ((Mask & (1<<8))!=0)
        {
			Pixel = (PackedPixels & 0x0f);
		}
		else
		{
            Pixel = pPixels[8];
		}

		PackedPixels = PackedPixels>>8;
		ScreenPixel = ConvertedColourTable[Pixel];

		if ((Mask & (1<<10))!=0)
        {
			Pixel = (PackedPixels & 0x0f);
		}
		else
		{
            Pixel = pPixels[10];
		}


		PackedPixels = PackedPixels>>8;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[2] = ScreenPixel;

		if ((Mask & (1<<12))!=0)
        {
			Pixel = (PackedPixels & 0x0f);
		}
		else
		{
            Pixel = pPixels[12];
		}

		PackedPixels = PackedPixels>>8;
		ScreenPixel = ConvertedColourTable[Pixel];

		if ((Mask & (1<<14))!=0)
        {
			Pixel = (PackedPixels & 0x0f);
		}
		else
		{
            Pixel = pPixels[14];
		}


		PackedPixels = PackedPixels>>8;
		ScreenPixel = ScreenPixel | (ConvertedColourTable[Pixel]<<16);
		((unsigned *)pScreen)[3] = ScreenPixel;


		pScreen+=(4*4);
        

    }

 
	Render_UpdateRenderAddress;
}
#endif



/* faster, but may not be accurate enough */
void    Render_Paletted_PutDataWordPLUS(int HorizontalCount,unsigned GraphicsData, int Line, unsigned Mask, int *pPixels)
{
    int i;

    PIXEL_DATA *pThisPixelData;

	Render_CalcRenderAddress(HorizontalCount,Line);

	GraphicsData = Render_ScrollGraphicsData(GraphicsData);

	/* Amiga port: B fast path when no sprite pixel lands on any pixel this
	 * PIXEL_STEP draws (only even mask bits are tested at PIXEL_STEP 2). */
#ifndef NO_B
	{
		unsigned Used = (PIXEL_STEP == 2) ? 0x5555 : 0xffff;
		if ((Mask & Used) == Used)
		{
			Render_FastPutByte(pScreen, (GraphicsData & 0x0ff00) >> 8);
			Render_FastPutByte(pScreen, GraphicsData & 0x0ff);
			Render_UpdateRenderAddress;
			return;
		}
	}
#endif

    {
        pThisPixelData = &pPixelData[(unsigned)(GraphicsData & 0x0ff00)>>8];

        for (i=0; i<8; i+=PIXEL_STEP)
        {
                int     Pixel;

                if ((Mask & (1<<i))!=0)
                {
                        Pixel = pThisPixelData->Pixel[i];
                }
                else
                {
                        Pixel = pPixels[i];
                }

                Render_Paletted_WriteColourToScreenBuffer(pScreen, Pixel);
                pScreen+=BytesPerPixel;
        }
    }

    {
        pThisPixelData = &pPixelData[(unsigned)(GraphicsData & 0x0ff)];

        for (i=0; i<8; i+=PIXEL_STEP)
        {
                int     Pixel;

                if ((Mask & (1<<(i+8)))!=0)
                {
                        Pixel = pThisPixelData->Pixel[i];
                }
                else
                {
                        Pixel = pPixels[i+8];
                }
                
                Render_Paletted_WriteColourToScreenBuffer(pScreen, Pixel);
                pScreen+=BytesPerPixel;
        }
    }

	Render_UpdateRenderAddress;
}
#if 0
void	my_memcpy(void *pDest, void *pSrc, unsigned long Length)
{
	/* align start */

	unsigned char *pDestPtr, *pSrcPtr;
		
	pDestPtr = pDest;
	pSrcPtr = pSrc;

	{
		int i;
		int LengthInLongs = Length>>2;

		for (i=0; i<LengthInLongs; i++)
		{
			((unsigned long*)pDestPtr)[0] = ((unsigned long*)pSrcPtr)[0];
			pDestPtr+=4; 
			pSrcPtr+=4;
		}
	}

	{
		int i;
		int BytesRemaining = Length - (Length  & (~0x02));

		for (i=0; i<BytesRemaining; i++)
		{
			((unsigned char *)pDestPtr)[0] = ((unsigned char *)pSrcPtr)[0];
			pDestPtr++;
			pSrcPtr++;
		}
	}
}
#endif
void	my_memcpy(void *pDest, void *pSrc, unsigned long Length)
{
	/* align start */
	unsigned char *pDestPtr, *pSrcPtr;
	
	pDestPtr = pDest;
	pSrcPtr = pSrc;

	{
		int i;
		int LengthInLongs = Length>>2;
		unsigned long Offset = 0;
		unsigned long *pLongSrcPtr = (unsigned long *)pSrcPtr, *pLongDestPtr = (unsigned long *)pDestPtr;

		for (i=0; i<LengthInLongs; i++)	
		{
			pLongDestPtr[Offset] = pLongSrcPtr[Offset];
		
			Offset++;
		}


	}
}





void    Render_DumpScreen4(void)
{
        unsigned char   *pScreen;
        unsigned char   *pScr;
        unsigned char   *pSurface;
        unsigned char   *pSurf;
        int i;
        int BytesPerPixel = ((CurrentGraphicsFormat.BPP+7)>>3);
        int Pitch;
        
        /* lock surface */
        if (Host_LockGraphicsBuffer())  
        {
                GRAPHICS_BUFFER_INFO *pGraphicsBufferInfo = Host_GetGraphicsBufferInfo();
                int XOffset;
                int XLength;
                int RenderXOffset;

                Pitch = pGraphicsBufferInfo->Pitch;     

                /* source address */
                pScreen = pScreenBase;
                /* dest address */
                pSurface = (unsigned char *)pGraphicsBufferInfo->pSurface;      

                /* if scanlines is true, this must be done before the pitch is adjusted. */
                pSurface += Render_ScreenYOffset*Pitch;

                if (ScanLines!=0)
                {
                        Pitch = Pitch<<1;
                }

                pScr = pScreen;
                pSurf = pSurface;

                pScr += Render_CPCYOffset*ScreenPitch;
                
                XOffset = Render_CPCXOffset * BytesPerPixel;
                XLength = Render_CPCRenderWidth * BytesPerPixel;
                RenderXOffset = Render_ScreenXOffset*BytesPerPixel;
                        
                for (i=0; i<Render_CPCRenderHeight; i++)
                {
                        pSurface = pSurf + RenderXOffset;
                        pScreen = pScr + XOffset;

						/*
						{
							unsigned long l;
							unsigned char *pSrcAddr = pScreen;
							unsigned char *pDestAddr = pSurface;
			
							for (l=0; l<XLength>>2; l++)
							{
								((unsigned long *)pDestAddr)[0] = ((unsigned long *)pSrcAddr)[0];
								((unsigned long *)pDestAddr)++;
								((unsigned long *)pSrcAddr)++;
							}

							for (l=0; l<(XLength&3); l++)
							{
								pDestAddr[0] = pSrcAddr[0];
								pDestAddr++;
								pSrcAddr++;
							}
						}
						*/
						// FIXME: SDL crashes here!
						/* Amiga port: copy only lines that changed and tell the host
						 * which ones (it converts only those to bitplanes). Compared a
						 * long at a time: libnix's memcmp is byte by byte. */
						if (Render_LineChangedHook && !FillScanLines && !(XLength & 3) &&
							!(((unsigned long)pSurface | (unsigned long)pScreen) & 3))
						{
							const unsigned int *s = (const unsigned int *)pScreen;
							unsigned int *d = (unsigned int *)pSurface;
							int n = XLength >> 2, k;
							for (k = 0; k < n; k++)
								if (s[k] != d[k]) break;
							if (k < n)
							{
								for (; k < n; k++) d[k] = s[k];
								Render_LineChangedHook(Render_ScreenYOffset + i);
							}
						}
						else
                        memcpy(pSurface, pScreen, XLength);

						if (FillScanLines)
						{
							pSurface+=Pitch>>1;
							my_memcpy(pSurface, pScreen, XLength);
						}

                        pScr+=ScreenPitch;

                        pSurf+=Pitch;
                }

                /* unlock surface */
                Host_UnlockGraphicsBuffer();    

                /* flip screen (in windowed mode performs a blit) */
                Host_SwapGraphicsBuffers();     

        }
}

void    Render_ClearDisplay(void)
{
        if (pScreenBase!=NULL)
        {
                memset(pScreenBase,0, ScreenPitch*(LINES_PER_SCREEN+1));
        }
}

#ifdef PAL_COUNT
int Render_PaletteUsedCount(void)
{
	int i, n = 0;
	for (i = 0; i < 256; i++) if (CPCPalette[i].Flags & PALETTE_ENTRY_USED) n++;
	return n;
}
#endif

/* Amiga port (18 Oct 2026): the paletted renderer gives each new CPC/Plus
 * colour one of 256 host palette entries and never frees them (the
 * Palette_Reset below is commented out upstream). Plus games set many of the
 * 4096 colours (Sonic GX uses 200 entries in 30 s), so after a game or two the
 * palette is full, new colours are dropped and drawn in a stale entry's colour:
 * corruption that grew with each game started by Power ON. When it is nearly
 * full, the palette is freed at the end of a frame (after that frame is out),
 * and the next frame takes entries for the colours it actually uses.
 * NO_PALGC_FIX: never freed, as before. */
#define PALGC_LIMIT 224
#ifndef PALGC_TOP
#define PALGC_TOP 64
#endif
#ifndef NO_PALGC_LOW
/* Free every entry and give the 32 pens entries again, in pen order, with the
 * exact colours they have now (Palette_Reset would recompute them from the
 * pens' hardware colours, and on a Plus a pen last set through the Gate Array
 * has a slightly different RGB from its ASIC colour, so the picture changed). */
static void Palette_Compact(void)
{
	unsigned char rgb[32][3];
	int i, have[32];
	for (i = 0; i < 32; i++)
	{
		int p = PaletteRemap[i];
		have[i] = (p >= 0 && p < 256);
		if (have[i]) { rgb[i][0] = CPCPalette[p].Red; rgb[i][1] = CPCPalette[p].Green; rgb[i][2] = CPCPalette[p].Blue; }
	}
	for (i = 0; i < 256; i++)
		if (!(CPCPalette[i].Flags & PALETTE_ENTRY_DO_NOT_REMOVE)) CPCPalette[i].Flags = 0;
	Render_ReserveBlack();
	for (i = 0; i < 32; i++)
	{
		int p = -1;
		if (have[i])
		{
			p = Palette_MatchColour(rgb[i][0], rgb[i][1], rgb[i][2]);
			if (p == -1) p = Palette_StoreColour(rgb[i][0], rgb[i][1], rgb[i][2]);
		}
		PaletteRemap[i] = p;
		if (i == 16) Render_PaletteIndexBorder = p >= 0 ? p : Render_PaletteIndexBlack;
	}
	Render_RemapStamp++;
	{ extern void CRTC_InvalidateRun(void); CRTC_InvalidateRun(); }   /* cached border index and pixels */
}
#endif
void    Render_DumpDisplay(void)
{
        pRender_DumpScreen();
#ifndef NO_PALGC_FIX
	if (Renderer_Active && BytesPerPixel == 1)   /* the paletted (8-bit) renderer */
	{
#ifndef NO_PALGC_LOW
		/* Sonic speed (20 Oct 2026): also freed as soon as an entry at or
		 * above PALGC_TOP (64) is taken, so a frame drawn with up to 64
		 * colours keeps indices below 64 and the Amiga c2p writes only
		 * bitplanes 0-5 (0-4 for lines whose colours are all below 32).
		 * Sonic GX uses about 57 colours a frame (mid-frame palette
		 * changes); 32 freed it every frame and reloaded the Amiga palette
		 * each time, 64 measured better on the A1200.
		 * After the free, the pens get entries again in pen order with the
		 * colours they have now (Palette_Compact), so a steady screen keeps
		 * the same indices.
		 * Only host entries (DO_NOT_REMOVE, e.g. the OSD pen 255) may sit
		 * above. Colours on screen are unchanged; only index numbers differ.
		 * NO_PALGC_LOW: freed only when 224 entries are in use, as before. */
		int i;
		for (i = PALGC_TOP; i < 256; i++)
			if ((CPCPalette[i].Flags & (PALETTE_ENTRY_USED | PALETTE_ENTRY_DO_NOT_REMOVE)) == PALETTE_ENTRY_USED)
				break;
#ifdef PAL_COUNT
		{ static int f, c; f++; if (i < 256) { c++; fprintf(stderr, "compact frame %d entry %d used %d\n", f, i, Render_PaletteUsedCount()); } }
#endif
		if (i < 256) Palette_Compact();
#else
		int i, n = 0;
		for (i = 0; i < 256; i++) if (CPCPalette[i].Flags & PALETTE_ENTRY_USED) n++;
		if (n >= PALGC_LIMIT) Palette_Reset();
#endif
	}
#endif
#ifdef PAL_COUNT
	{ static int f; if (++f % 100 == 0) fprintf(stderr, "frame %d palette entries used %d\n", f, Render_PaletteUsedCount()); }
#endif

/*		Palette_Set();

		Palette_Reset(); */
}





void    Render_Initialise(void)
{
        CPC_BuildModeRenderTables();

	pScreenBase = NULL;
	ScreenPitch = 0;
	Renderer_Active = FALSE;

	Palette_Initialise();

	Render_SetRenderingAccuracyForWindowedMode(RENDERING_ACCURACY_LOW);
}

  


void    Render_SetRenderingAccuracy(int Accuracy)
{
        if (Accuracy == RENDERING_ACCURACY_LOW)
        {
                /* mis out every other pixel in x to maintain image aspect,
                screen is smaller */
                PIXEL_STEP = 2; Render_RemapStamp++;
                PIXEL_STEP_SHIFT = 1;
                ScanLines = 0;
				FillScanLines = 0;
        }

        if (Accuracy == RENDERING_ACCURACY_HIGH)
        {
                /* show every pixel, but render black lines between each graphical
                line to maintain image aspect - closer to tv image with this mode */
                PIXEL_STEP=1; Render_RemapStamp++;
                PIXEL_STEP_SHIFT=0;
                ScanLines = 1;
				FillScanLines = 0;
        }

        if (Accuracy == RENDERING_ACCURACY_HIGHER)
        {
                /* show every pixel, but render black lines between each graphical
                line to maintain image aspect - closer to tv image with this mode */
                PIXEL_STEP=1; Render_RemapStamp++;
                PIXEL_STEP_SHIFT=0;
                ScanLines = 1;
				FillScanLines = 1;
        }

}

/* Troels K. begin */
/*static*/ int Render_RenderingAccuracyForWindowedMode = RENDERING_ACCURACY_LOW;
/* Troels K. end */


void	Render_SetRenderingAccuracyForWindowedMode(int Accuracy)
{
	Render_RenderingAccuracyForWindowedMode = Accuracy;

	Render_SetRenderingAccuracy(Accuracy);
}


/* ScreenResX, ScreenResY define rendering window dimensions.
 In FullScreen mode these are defined by the selected screen resolution.
 In Windowed mode these can be changed to allow for full-display */
BOOL InitialiseRender(int ScreenResX, int ScreenResY, int BPP)
{
    int ScreenWidth;

    if (pScreenBase!=NULL)
    {
            free(pScreenBase);
            pScreenBase = NULL;
			ScreenPitch = 0;
	}


    ScreenWidth = BITS_PER_LINE;    
    ScreenHeight = LINES_PER_SCREEN+1;      

    ScreenPitch = (unsigned)ScreenWidth * ((BPP+7)>>3);
    ScreenPitch += ((4-(ScreenPitch & 0x03)) & 0x03);

    pScreenBase = (unsigned char *)malloc(ScreenPitch * ScreenHeight);

    BytesPerPixel = (BPP+7)>>3;

    {
        int CPCXOffset, CPCYOffset;
        int CPCScreenWidth, CPCScreenHeight;
        int ActualCPCScreenHeight;


        CPCXOffset = X_CRTC_CHAR_OFFSET<<(1+3);
        CPCYOffset = Y_CRTC_LINE_OFFSET;
        CPCScreenWidth = X_CRTC_CHAR_WIDTH<<(1+3);
        CPCScreenHeight = Y_CRTC_LINE_HEIGHT;

        CPCScreenWidth = CPCScreenWidth>>PIXEL_STEP_SHIFT;
        CPCXOffset = CPCXOffset>>PIXEL_STEP_SHIFT;

        if (ScreenResX>=CPCScreenWidth)
        {
                /* screen resolution is higher than viewable CPC screen */
                
                /* centralise screen in display area */
                Render_ScreenXOffset = (ScreenResX>>1)-(CPCScreenWidth>>1);
                Render_CPCRenderWidth = CPCScreenWidth; 
                Render_CPCXOffset = CPCXOffset;
        }
        else
        {
                /* screen resolution is lower than viewable CPC screen */
                /* clip screen display */

                /* render from left hand side */
                Render_ScreenXOffset = 0;
                /* adjust width we are going to render */
                Render_CPCRenderWidth = ScreenResX;
                /* set new offset, so that the viewable area is still central */
                Render_CPCXOffset = CPCXOffset + (CPCScreenWidth>>1) - (ScreenResX>>1);
        }

        if (ScanLines!=0)
        {
                ActualCPCScreenHeight = CPCScreenHeight<<1;
        }
        else
        {
                ActualCPCScreenHeight = CPCScreenHeight;
        }

        if (ScreenResY>=ActualCPCScreenHeight)
        {
                /* screen Y resolution is higher than viewable CPC screen */
                /* centralise screen in display area */

                Render_ScreenYOffset = (ScreenResY>>1)-(ActualCPCScreenHeight>>1);
                Render_CPCRenderHeight = CPCScreenHeight;
                Render_CPCYOffset = CPCYOffset;
        }
        else
        {
                /* screen Y resolution is less than viewable CPC screen */
                /* clip screen display */

                /* render from top */
                Render_ScreenYOffset = 0;
                /* adjust height we are going to render */
                Render_CPCRenderHeight = ScreenResY;

                if (ScanLines)
                {
                        Render_CPCRenderHeight = Render_CPCRenderHeight>>1;
                }

                /* set new offset, so that viewablew area is still central */
                Render_CPCYOffset = CPCYOffset + ((ActualCPCScreenHeight - ScreenResY)>>1);
        }
    }       

	/* Amiga port: the part of each row Render_DumpScreen4 copies out, in bytes,
	 * and the rows it copies (see Render_SkipCellIfHidden). */
	{ extern void CRTC_InvalidateRun(void); CRTC_InvalidateRun(); }   /* render geometry changed */
	Render_VisX0 = Render_CPCXOffset * BytesPerPixel;
	Render_VisX1 = Render_VisX0 + Render_CPCRenderWidth * BytesPerPixel;
	Render_VisTop = Render_CPCYOffset;
	Render_VisLines = (ScanLines != 0) ? 0x7fffffff : Render_CPCRenderHeight;
	Render_CellBytes = (16 >> PIXEL_STEP_SHIFT) * BytesPerPixel;

	if (pScreenBase!=NULL)
	{
		memset(pScreenBase, 0, ScreenPitch*ScreenHeight);
		Renderer_Active = TRUE;
		Render_FirstLine();
	}
	else
	{
		Renderer_Active = FALSE;
	}



	return Renderer_Active;
}

BOOL	Render_IsRenderActive(void)
{
	return Renderer_Active;
}

BOOL Render_SetDisplayFullScreen(int Width, int Height, int Depth)
{
        
        /* not recommended for any res as low as 320x240 */
 
        int ScreenResX = Width;
        int ScreenResY = Height;
        int ScreenDepth = Depth;

        Render_SetRenderingAccuracy(RENDERING_ACCURACY_HIGH);

        if (Host_SetDisplay(DISPLAY_TYPE_FULLSCREEN,ScreenResX,ScreenResY,ScreenDepth))
        {
                GRAPHICS_BUFFER_COLOUR_FORMAT   *pGraphicsBufferColourFormat = Host_GetGraphicsBufferColourFormat();

                CurrentGraphicsFormat.BPP = pGraphicsBufferColourFormat->BPP;
                
                CurrentGraphicsFormat.Red.BPP = pGraphicsBufferColourFormat->Red.BPP;
                CurrentGraphicsFormat.Red.Mask = pGraphicsBufferColourFormat->Red.Mask;
                CurrentGraphicsFormat.Red.Shift = pGraphicsBufferColourFormat->Red.Shift;
                
                CurrentGraphicsFormat.Green.BPP = pGraphicsBufferColourFormat->Green.BPP;
                CurrentGraphicsFormat.Green.Mask = pGraphicsBufferColourFormat->Green.Mask;
                CurrentGraphicsFormat.Green.Shift = pGraphicsBufferColourFormat->Green.Shift;
                
                CurrentGraphicsFormat.Blue.BPP = pGraphicsBufferColourFormat->Blue.BPP;
                CurrentGraphicsFormat.Blue.Mask = pGraphicsBufferColourFormat->Blue.Mask;
                CurrentGraphicsFormat.Blue.Shift = pGraphicsBufferColourFormat->Blue.Shift;

				if (CurrentGraphicsFormat.BPP!=8)
				{
					Render_TrueColourRGB_Setup();
				}
				else
				{
					Render_Paletted_Setup();
				}

                pRender_DumpScreen = Render_DumpScreen4;

                if (InitialiseRender(ScreenResX,ScreenResY,ScreenDepth))
				{
					return TRUE;
				}
				
				return FALSE;
        }

        return FALSE;
}


BOOL    Render_SetDisplayWindowed(void)
{
        int ScreenWidth, ScreenHeight;

        Render_SetRenderingAccuracy(Render_RenderingAccuracyForWindowedMode);

        ScreenWidth = (X_CRTC_CHAR_WIDTH<<(1+3))>>PIXEL_STEP_SHIFT;     
        ScreenHeight = (Y_CRTC_LINE_HEIGHT);

        /* adjust height for scanlines */
        if (ScanLines)
        {
                ScreenHeight = ScreenHeight<<1;
        }
        
        if (Host_SetDisplay(DISPLAY_TYPE_WINDOWED,ScreenWidth, ScreenHeight,0))
        {
                GRAPHICS_BUFFER_COLOUR_FORMAT   *pGraphicsBufferColourFormat = Host_GetGraphicsBufferColourFormat();

                CurrentGraphicsFormat.BPP = pGraphicsBufferColourFormat->BPP;
                
                CurrentGraphicsFormat.Red.BPP = pGraphicsBufferColourFormat->Red.BPP;
                CurrentGraphicsFormat.Red.Mask = pGraphicsBufferColourFormat->Red.Mask;
                CurrentGraphicsFormat.Red.Shift = pGraphicsBufferColourFormat->Red.Shift;
                
                CurrentGraphicsFormat.Green.BPP = pGraphicsBufferColourFormat->Green.BPP;
                CurrentGraphicsFormat.Green.Mask = pGraphicsBufferColourFormat->Green.Mask;
                CurrentGraphicsFormat.Green.Shift = pGraphicsBufferColourFormat->Green.Shift;
                
                CurrentGraphicsFormat.Blue.BPP = pGraphicsBufferColourFormat->Blue.BPP;
                CurrentGraphicsFormat.Blue.Mask = pGraphicsBufferColourFormat->Blue.Mask;
                CurrentGraphicsFormat.Blue.Shift = pGraphicsBufferColourFormat->Blue.Shift;

                pRender_DumpScreen = Render_DumpScreen4;        

				if (CurrentGraphicsFormat.BPP!=8)
				{
					Render_TrueColourRGB_Setup();
				}
				else
				{
					Render_Paletted_Setup();
				}
                
				if (InitialiseRender(ScreenWidth, ScreenHeight, CurrentGraphicsFormat.BPP))
				{
					return TRUE;
				}

				return FALSE;
        }

        return FALSE;
}


void    Render_GetPixelRGBAtXY(int X,int Y, unsigned char *r, unsigned char *g, unsigned char *b)
{
    int     PixelBytes = (CurrentGraphicsFormat.BPP+7)>>3;
    unsigned PackedPixelData;
    
    unsigned char *pScreenData = (unsigned char *)pScreenBase + ((Y+Y_CRTC_LINE_OFFSET)*ScreenPitch) + (((X_CRTC_CHAR_OFFSET<<(1+3))>>PIXEL_STEP_SHIFT)*PixelBytes) + (X*PixelBytes);
    unsigned localr, localg, localb;

    PackedPixelData = ReadPackedImageData(pScreenData, PixelBytes);

    GetRGBFromSourceImageFormat(PackedPixelData, &localr, &localg, &localb, &CurrentGraphicsFormat);

    *r = (unsigned char)localr;
    *g = (unsigned char)localg;
    *b = (unsigned char)localb;
}


static int RenderLine=0;
unsigned char *pRowStart;       /* Amiga port: start of the row being drawn */
int RenderRowVisible = 1;

static void Render_RowChanged(void)
{
	pRowStart = pScreenLine;
	RenderRowVisible = (unsigned)(RenderLine - Render_VisTop) < (unsigned)Render_VisLines;
}

/* Amiga port: called for each cell Graphics_Update would draw. Cells are drawn
 * at a running pointer (LESS_MULTS), so a cell that can't be seen still moves
 * it on. The cell just left of the window is drawn too: the border routine
 * writes 3 bytes past its cell. Returns 1 if the cell was skipped. */
int Render_SkipCellIfHidden_Unused(void)   /* kept for reference; crtc.c has the inline copy */
{
	int col = (int)(pScreenLine - pRowStart);
	if (RenderRowVisible && col + Render_CellBytes >= Render_VisX0 && col < Render_VisX1)
		return 0;
	pScreenLine += Render_CellBytes;
	return 1;
}

void    Render_FirstLine(void)
{
        RenderLine = 0;

        pScreenLine = pScreenBase;
		Render_RowChanged();
}
/*
void    Render_SetLineStart(void)
{
        pScreenLine = pScreenBase + (RenderLine*ScreenPitch);
}
*/
void    Render_NextLine(void)
{
        RenderLine++;

		if (RenderLine>=ScreenHeight)
		{
			RenderLine = 0;
		}

        pScreenLine = pScreenBase + (RenderLine*ScreenPitch);
		Render_RowChanged();
}

/* ---------------------------------------------------------- */
#if 0
/* Put a pixel to the screen - used by Plus Sprite rendering routines */
static void    Render_PutPixel(int X, int Y, int Pixel)
{
        int     ScreenOffset = (Y*ScreenPitch) + (X>>PIXEL_STEP_SHIFT)*BytesPerPixel;
        unsigned char *pScreen = pScreenBase + ScreenOffset;

        
        Render_TrueColourRGB_WriteColourToScreenBuffer(pScreen, Pixel);
}
#endif

#if 0
extern char *FontGraphics;

void    PlotPixel(int X, int Y)
{
        int             Offset = (ScreenPitch * (Y+Render_CPCYOffset)) + (Render_CPCXOffset * BytesPerPixel) + (X * BytesPerPixel);
        unsigned char *pScreen = pScreenBase + Offset;

        unsigned Pixel = 0x0fffffff;

        ((unsigned *)pScreen)[0] = Pixel;


}


/* plot a char on the display */
void    PlotChar(char ch, int CharX, int CharY)
{
        int PixelX = CharX<<3, PixelY = CharY<<3;
    char *pCharGfx = &FontGraphics[(ch<<3)];
    int y;

    for (y=0; y<8; y++)
    {
        int bit;
        char bits;
        
        bits = pCharGfx[y];
        
        for (bit=0; bit<8; bit++)
        {
            int PenIndex;
            
            if (bits & 0x080)
                PenIndex = 2;
            else
                PenIndex = 0;

            bits = bits<<1;
            

                        PlotPixel(PixelX + bit, PixelY + y);

       }
    }
}


/* Plot a text string to graphics display */
void    Render_PlotText(char *pString, int X, int Y)
{
        int ScreenResX = 800, ScreenResY = 600;
    char *pStringPtr = pString;
        int XCharCoord, YCharCoord;

        XCharCoord = X;
        YCharCoord = Y;

    do
    {
        char ch;
        
                /* get char */
        ch = pStringPtr[0];

        pStringPtr++;

                /* if char is 0, then end of string reached, and quit */
        if (ch==0)
             return;

                /* Plot this character */
        PlotChar(ch, XCharCoord, YCharCoord);

        XCharCoord++;

                /* update char position */
                if (XCharCoord>=(ScreenResX>>3))
                {
                        YCharCoord++;
                        XCharCoord = 0;

                        if (YCharCoord>=(ScreenResY>>3))
                        {
                                YCharCoord = 0;
                        }
                }

    }
    while (1==1);
}
#endif

BOOL	Render_IsRendererActive(void)
{
	return Renderer_Active;
}





        
