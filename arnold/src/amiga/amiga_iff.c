/* Arnold on AmigaOS: show an IFF ILBM picture (the keyboard map) in a window
 * on the menu screen. Uncompressed or ByteRun1, up to 8 planes; the picture's
 * CMAP is loaded into the screen's pens 0..n (the menu uses pens 0-3 too, so
 * the picture is drawn with the same four colours it was made with). */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include "amiga_sys.h"

static ULONG be32(const UBYTE *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3]; }
static UWORD be16(const UBYTE *p) { return (p[0] << 8) | p[1]; }

/* Decode into an allocated BitMap; returns NULL on any problem. */
static struct BitMap *load_ilbm(const char *name, int *pw, int *ph, UBYTE *cmap, int *ncol)
{
	FILE *f = fopen(name, "rb");
	long size; UBYTE *buf, *p, *end;
	int w = 0, h = 0, depth = 0, comp = 0;
	struct BitMap *bm = NULL;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
	buf = AllocVec(size, MEMF_ANY);
	if (!buf || fread(buf, 1, size, f) != (size_t)size) { fclose(f); if (buf) FreeVec(buf); return NULL; }
	fclose(f);
	if (size < 12 || memcmp(buf, "FORM", 4) || memcmp(buf + 8, "ILBM", 4)) { FreeVec(buf); return NULL; }
	p = buf + 12; end = buf + size; *ncol = 0;
	while (p + 8 <= end) {
		ULONG len = be32(p + 4); UBYTE *d = p + 8;
		if (!memcmp(p, "BMHD", 4)) { w = be16(d); h = be16(d + 2); depth = d[8]; comp = d[10]; }
		else if (!memcmp(p, "CMAP", 4)) { *ncol = len / 3; if (*ncol > 256) *ncol = 256; memcpy(cmap, d, *ncol * 3); }
		else if (!memcmp(p, "BODY", 4) && w && h && depth <= 8) {
			int rowbytes = ((w + 15) / 16) * 2, y, pl;
			UBYTE *s = d, *se = d + len;
			bm = AllocBitMap(w, h, depth, BMF_CLEAR, NULL);
			if (!bm) break;
			for (y = 0; y < h; y++) for (pl = 0; pl < depth; pl++) {
				UBYTE *row = bm->Planes[pl] + (LONG)y * bm->BytesPerRow;
				int n = 0;
				if (comp == 0) { if (s + rowbytes > se) break; memcpy(row, s, rowbytes); s += rowbytes; }
				else while (n < rowbytes && s < se) {
					signed char c = (signed char)*s++;
					if (c >= 0) { int k = c + 1; if (s + k > se) break; if (n + k > rowbytes) k = rowbytes - n; memcpy(row + n, s, k); s += c + 1; n += k; }
					else if (c != -128) { int k = 1 - c; UBYTE v = *s++; if (n + k > rowbytes) k = rowbytes - n; memset(row + n, v, k); n += k; }
				}
			}
			break;
		}
		p = d + len + (len & 1);
	}
	FreeVec(buf);
	*pw = w; *ph = h;
	return bm;
}

/* Modal: shows the picture on the given screen until a key or mouse click. */
void Sys_ShowPicture(void *screen, const char *name)
{
	struct Screen *scr = (struct Screen *)screen;
	struct Window *win;
	struct BitMap *bm;
	UBYTE cmap[768];
	int w, h, ncol, i;
	ULONG pal[2 + 256 * 3];
	bm = load_ilbm(name, &w, &h, cmap, &ncol);
	if (!bm) { Sys_MenuError("Can't load the keyboard picture\n(Keyboard.iff beside Arnold)"); return; }
	win = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)scr,
		WA_Left, (scr->Width - w) / 2 < 0 ? 0 : (scr->Width - w) / 2, WA_Top, scr->BarHeight + 1,
		WA_Width, w > scr->Width ? scr->Width : w, WA_Height, h,
		WA_Borderless, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE, WA_SimpleRefresh, TRUE,
		WA_IDCMP, IDCMP_RAWKEY | IDCMP_MOUSEBUTTONS | IDCMP_EXTENDEDMOUSE, TAG_DONE);
	if (win) {
		if (ncol > 0 && ncol <= (1 << scr->RastPort.BitMap->Depth)) {
			pal[0] = ((ULONG)ncol << 16);
			for (i = 0; i < ncol; i++) { pal[1 + i * 3] = cmap[i * 3] * 0x01010101UL; pal[2 + i * 3] = cmap[i * 3 + 1] * 0x01010101UL; pal[3 + i * 3] = cmap[i * 3 + 2] * 0x01010101UL; }
			pal[1 + ncol * 3] = 0;
			LoadRGB32(&scr->ViewPort, pal);
		}
		BltBitMapRastPort(bm, 0, 0, win->RPort, 0, 0, win->Width, win->Height, 0xc0);
		for (;;) {
			struct IntuiMessage *msg;
			int done = 0;
			WaitPort(win->UserPort);
			while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort))) {
				if (msg->Class == IDCMP_MOUSEBUTTONS || (msg->Class == IDCMP_RAWKEY && !(msg->Code & 0x80))) done = 1;
				ReplyMsg((struct Message *)msg);
			}
			if (done) break;
		}
		CloseWindow(win);
	}
	FreeBitMap(bm);
}
