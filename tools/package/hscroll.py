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

def render(s):
    # the scroll function, next to the mode tables
    s = rep(s, 'int CurrentTopShift;', '''int CurrentTopShift;
static int HorizontalPixelScroll = 0;

/* Amiga port (15 Oct 2026): Plus horizontal soft scroll in the paletted
 * renderer. The scroll delays the pixel stream by whole pixels of the current
 * mode (ModeHorizontalPixelScroll = dots >> mode shift). The paletted path did
 * a plain bit shift of the two graphics bytes, which is right only in mode 2:
 * in modes 0 and 1 a pixel's bits are interleaved across the byte, so the
 * shift mixed bits of neighbouring pixels (outlines right, colours scrambled:
 * Sonic GX throughout, Ghosts'n Goblins at times). This is the per-pixel shift
 * Arnold's true-colour renderer already uses. NO_HSCROLL_FIX: the old shift. */
unsigned int Render_ScrollGraphicsData(unsigned int Data)
{
	int i;
#ifdef NO_HSCROLL_FIX
	return Data >> ModeHorizontalPixelScroll;
#else
	if (CurrentMode == 2)
		return Data >> ModeHorizontalPixelScroll;
	for (i = 0; i < ModeHorizontalPixelScroll; i++)
		Data = ((Data & (unsigned int)CurrentModeMask) >> 1) | ((Data & ~(unsigned int)CurrentModeMask) >> CurrentTopShift);
	return Data;
#endif
}''')
    s = rep(s, 'static int HorizontalPixelScroll = 0;\n\nvoid    Render_SetHorizontalPixelScroll', 'void    Render_SetHorizontalPixelScroll')
    # keep the pixel count in step with the mode (was only recomputed on a scroll write)
    s = rep(s, '''                        CurrentModeMask = 0x07f7f7f7f;
                        CurrentModeShift = 0;
                        CurrentTopShift = 1;
						break;
        }''', '''                        CurrentModeMask = 0x07f7f7f7f;
                        CurrentModeShift = 0;
                        CurrentTopShift = 1;
						break;
        }
#ifndef NO_HSCROLL_FIX
        ModeHorizontalPixelScroll = HorizontalPixelScroll >> CurrentModeShift;
#endif''')
    s = rep(s, '''	Render_CalcRenderAddress(HorizontalCount,Line);

	GraphicsData = GraphicsData>>ModeHorizontalPixelScroll;

	/* Amiga port: B fast path''', '''	Render_CalcRenderAddress(HorizontalCount,Line);

	GraphicsData = Render_ScrollGraphicsData(GraphicsData);

	/* Amiga port: B fast path''')
    return s
rw('render5.c', render)

def crtc(s):
    s = rep(s, 'extern int Render_PaletteIndexBlack, PIXEL_STEP, ModeHorizontalPixelScroll;',
               'extern int Render_PaletteIndexBlack, PIXEL_STEP, ModeHorizontalPixelScroll;\nextern unsigned int Render_ScrollGraphicsData(unsigned int Data);')
    s = rep(s, '''unsigned Data = (unsigned)CRTC_InternalState.GraphicsLong >> ModeHorizontalPixelScroll;''',
               '''unsigned Data = ModeHorizontalPixelScroll ? Render_ScrollGraphicsData((unsigned)CRTC_InternalState.GraphicsLong) : (unsigned)CRTC_InternalState.GraphicsLong;''')
    s = rep(s, '''unsigned Data = (unsigned)glong >> scroll;''',
               '''unsigned Data = scroll ? Render_ScrollGraphicsData((unsigned)glong) : (unsigned)glong;''')
    return s
rw('crtc.c', crtc)
print('ok')
