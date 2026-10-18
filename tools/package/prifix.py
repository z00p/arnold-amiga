import os
REPO_CPC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'arnold', 'src', 'cpc')
os.chdir(REPO_CPC)
CRLF = chr(13) + chr(10)

def rw(p, f):
    s = open(p, newline='').read(); cr = CRLF in s; s = s.replace(CRLF, '\n')
    s2 = f(s); assert s2 != s, p
    open(p, 'w', newline='').write(s2.replace('\n', CRLF) if cr else s2)

def rep(s, a, b):
    assert s.count(a) == 1, a
    return s.replace(a, b)

def asic(s):
    # 1. the PRI compare moves out of ASIC_HSync (HSYNC end) into its own function
    s = rep(s, '''        /* is raster interrupt line active? */
        if (ASIC_Data.ASIC_RasterInterruptLine!=0)
        {
                if ((CRTC_LineCounter == ((ASIC_Data.ASIC_RasterInterruptLine>>3) & 0x01f)) &&
                        (CRTC_RasterCounter == (ASIC_Data.ASIC_RasterInterruptLine & 0x07)))
                {
                        /* raster interrupt occured */''', '''#ifdef NO_PRI_FIX
        /* is raster interrupt line active? */
        if (ASIC_Data.ASIC_RasterInterruptLine!=0)
        {
                if ((CRTC_LineCounter == ((ASIC_Data.ASIC_RasterInterruptLine>>3) & 0x01f)) &&
                        (CRTC_RasterCounter == (ASIC_Data.ASIC_RasterInterruptLine & 0x07)))
                {
                        /* raster interrupt occured */''')
    s = rep(s, '''                        ASIC_SetRasterInterrupt();
                }
        }

        /* do a dma cycle and maybe set a dma interrupt */''', '''                        ASIC_SetRasterInterrupt();
                }
        }
#endif

        /* do a dma cycle and maybe set a dma interrupt */''')
    s = rep(s, '''void    ASIC_HSync(int CRTC_LineCounter, int CRTC_RasterCounter)
{''', '''/* Amiga port (15 Oct 2026): the programmable raster interrupt (PRI) is raised
 * when HSYNC starts on the matching line, not when it ends as the CPC's own
 * 52-line interrupt is (as CPCEC does). Raising it ~R3 characters late made
 * Sonic GX's chain of PRI interrupts (each handler sets PRI to the next line)
 * miss its next line: its split screens and palette changes stopped. Writing
 * PRI equal to the current line while HSYNC is active raises it at once.
 * NO_PRI_FIX restores the old timing. */
#ifndef NO_PRI_FIX
static int ASIC_PRI_Matches(int CRTC_LineCounter, int CRTC_RasterCounter)
{
	return ASIC_Data.ASIC_RasterInterruptLine != 0 &&
		CRTC_LineCounter == ((ASIC_Data.ASIC_RasterInterruptLine >> 3) & 0x01f) &&
		CRTC_RasterCounter == (ASIC_Data.ASIC_RasterInterruptLine & 0x07);
}
static void     ASIC_TriggerInterrupt(void);
void    ASIC_HSyncStart(int CRTC_LineCounter, int CRTC_RasterCounter)
{
	if ((ASIC_Data.Flags & ASIC_ENABLED) == 0)
		return;
	if (ASIC_PRI_Matches(CRTC_LineCounter, CRTC_RasterCounter))
	{
#ifdef TRACE_HW
		{ extern void Trace_Hw(const char *, int, int); Trace_Hw("PRI fires (hsync start)", ASIC_Data.ASIC_RasterInterruptLine, 0); }
#endif
		ASIC_SetRasterInterrupt();
		ASIC_TriggerInterrupt();
	}
}
#endif

void    ASIC_HSync(int CRTC_LineCounter, int CRTC_RasterCounter)
{''')
    s = rep(s, '''				case 0:
				{
					ASIC_Data.ASIC_RasterInterruptLine = (unsigned char)Data;
					return;
				}''', '''				case 0:
				{
					ASIC_Data.ASIC_RasterInterruptLine = (unsigned char)Data;
#ifndef NO_PRI_FIX
					{
						extern int CRTC_InHSync(int *Line, int *Raster);
						int l, r;
						if (CRTC_InHSync(&l, &r) && ASIC_PRI_Matches(l, r))
						{
							ASIC_SetRasterInterrupt();
							ASIC_TriggerInterrupt();
						}
					}
#endif
					return;
				}''')
    return s
rw('asic.c', asic)

def crtc(s):
    s = rep(s, '''                                    /* enable horizontal sync counter */
                                    CRTC_SetFlag(CRTC_HS_FLAG);
''', '''                                    /* enable horizontal sync counter */
                                    CRTC_SetFlag(CRTC_HS_FLAG);
#ifndef NO_PRI_FIX
                                    if (CPCHardware == CPC_HW_CPCPLUS)
                                    {
                                            extern void ASIC_HSyncStart(int, int);
                                            ASIC_HSyncStart(CRTC_InternalState.LineCounter, CRTC_InternalState.RasterCounter);
                                    }
#endif
''')
    s = rep(s, '''CRTC_INTERNAL_STATE *CRTC_GetInternalState(void)
{''', '''/* Amiga port: for the ASIC's PRI write (see asic.c ASIC_HSyncStart) */
int CRTC_InHSync(int *Line, int *Raster)
{
	*Line = CRTC_InternalState.LineCounter;
	*Raster = CRTC_InternalState.RasterCounter;
	return (CRTC_InternalState.CRTC_Flags & CRTC_HS_FLAG) != 0;
}

CRTC_INTERNAL_STATE *CRTC_GetInternalState(void)
{''')
    return s
rw('crtc.c', crtc)
print('ok')
