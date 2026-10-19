/* Arnold on AmigaOS: the menu screen (GadTools), styled after the OS 3.2 prefs
 * editors. 17 Oct 2026 (John): the settings live in pull-down menus (Options,
 * Controls) and the window keeps what changes from game to game: the media,
 * the per-game buttons and the Reset / Power ON / Continue / Exit row, with a
 * read-only summary of the current settings. Help in the emulation brings it
 * up; Continue goes back. OS side only: no Arnold headers (see amiga_sys.h). */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <graphics/modeid.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>
#include "amiga_sys.h"
#include "amiga_logo.h"

struct Library *GadToolsBase, *AslBase;
int Sys_MenuKeyLog;   /* host sets it with KEYLOG: raw keys seen by the menu go to PROGDIR:menukeys.log */
static FILE *mkeylog;

enum { G_CART, G_CARTLOAD, G_CARTEJECT, G_DISKA, G_DISKALOAD, G_DISKAEJECT,
       G_DISKB, G_DISKBLOAD, G_DISKBEJECT,
       G_RESET, G_POWER, G_CONTINUE, G_QUIT, G_SAVEGAME, G_FORGETGAME, G_KEYS, G_SHOT, G_COUNT };

/* menu item ids (nm_UserData) */
enum { M_NONE, M_LOADCART, M_LOADA, M_LOADB, M_EJECTALL, M_KEYS, M_SHOT, M_SAVE, M_SAVEGAME, M_FORGETGAME, M_QUIT,
       M_SOUND, M_HIRES, M_SKIP0, M_SKIP1, M_SKIP2, M_SKIP3, M_SKIP5, M_REF1, M_REF2, M_REF3, M_REF4,
       M_JOY0, M_JOY1, M_JOY2, M_PAD0, M_PAD1 };

/* Pens of the 16-colour menu screen (16 Oct 2026, styled after the OS 3.2
 * prefs editors at John's request): the classic grey / black / white / blue
 * Workbench look with black text, 3D bevels and a blue active title bar.
 * Pens 8-15 are the GX4000 picture in the logo (amiga_logo.c). */
enum { P_BG = 0, P_BLACK, P_WHITE, P_BLUE, P_RED, P_DARK, P_LIGHT, P_DBLUE };
static ULONG menupal[2 + 16 * 3] = { 16 << 16,
	0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA,   /* 0 grey: background */
	0x00000000, 0x00000000, 0x00000000,   /* 1 black: text, shadows */
	0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,   /* 2 white: shine, highlighted text */
	0x66666666, 0x88888888, 0xBBBBBBBB,   /* 3 blue: active title bar, selections */
	0xDDDDDDDD, 0x22222222, 0x22222222,   /* 4 red: the logo's stripe */
	0x77777777, 0x77777777, 0x77777777,   /* 5 dark grey */
	0xCCCCCCCC, 0xCCCCCCCC, 0xCCCCCCCC,   /* 6 light grey */
	0x33333333, 0x55555555, 0x88888888,   /* 7 dark blue */
	0 };
/* DrawInfo pens as the 3.x defaults: black text, white shine, blue fill */
static UWORD menupens[] = { 0, 1, 1, 2, 1, 3, 1, 0, 2, 1, 2, 1, (UWORD)~0 };

static struct Screen *mscr;
static struct Window *mwin;
static struct Gadget *glist, *gad[G_COUNT];
static struct Menu *menustrip;
static APTR vi;
static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, 0, 0 };
static const char *skip_labels[] = { "Off", "Up to 1", "Up to 2", "Up to 3", "Up to 5" };
static const int skip_values[] = { 0, 1, 2, 3, 5 };
static const char *refresh_labels[] = { "All", "1 in 2", "1 in 3", "1 in 4" };
static const char *joykey_labels[] = { "Off", "Cursors+Z/X", "QAOP+Space/M" };
static const char *pad_labels[] = { "Normal", "CD32" };

/* The pull-down menus. Checked items are set from the settings before
 * CreateMenus; the sub-items of a radio group exclude each other. */
#define X(i) (~(1UL << (i)))
static struct NewMenu nm[] = {
	{ NM_TITLE, (STRPTR)"Project", NULL, 0, 0, NULL },
	{ NM_ITEM, (STRPTR)"Load cartridge...", (STRPTR)"C", 0, 0, (APTR)M_LOADCART },
	{ NM_ITEM, (STRPTR)"Load disk A...", (STRPTR)"A", 0, 0, (APTR)M_LOADA },
	{ NM_ITEM, (STRPTR)"Load disk B...", (STRPTR)"B", 0, 0, (APTR)M_LOADB },
	{ NM_ITEM, (STRPTR)"Eject all", NULL, 0, 0, (APTR)M_EJECTALL },
	{ NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
	{ NM_ITEM, (STRPTR)"Keyboard map...", (STRPTR)"K", 0, 0, (APTR)M_KEYS },
	{ NM_ITEM, (STRPTR)"Screenshot", (STRPTR)"X", 0, 0, (APTR)M_SHOT },
	{ NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
	{ NM_ITEM, (STRPTR)"Save Prefs", (STRPTR)"S", 0, 0, (APTR)M_SAVE },
	{ NM_ITEM, (STRPTR)"Save for game", NULL, 0, 0, (APTR)M_SAVEGAME },
	{ NM_ITEM, (STRPTR)"Forget for game", NULL, 0, 0, (APTR)M_FORGETGAME },
	{ NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
	{ NM_ITEM, (STRPTR)"Quit", (STRPTR)"Q", 0, 0, (APTR)M_QUIT },
	{ NM_TITLE, (STRPTR)"Options", NULL, 0, 0, NULL },
	{ NM_ITEM, (STRPTR)"Sound (Paula)", NULL, CHECKIT | MENUTOGGLE, 0, (APTR)M_SOUND },
	{ NM_ITEM, (STRPTR)"Hires screen", NULL, CHECKIT | MENUTOGGLE, 0, (APTR)M_HIRES },
	{ NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
	{ NM_ITEM, (STRPTR)"Frame skip", NULL, 0, 0, NULL },
	{ NM_SUB, (STRPTR)"Off", NULL, CHECKIT, X(0), (APTR)M_SKIP0 },
	{ NM_SUB, (STRPTR)"Up to 1", NULL, CHECKIT, X(1), (APTR)M_SKIP1 },
	{ NM_SUB, (STRPTR)"Up to 2", NULL, CHECKIT, X(2), (APTR)M_SKIP2 },
	{ NM_SUB, (STRPTR)"Up to 3", NULL, CHECKIT, X(3), (APTR)M_SKIP3 },
	{ NM_SUB, (STRPTR)"Up to 5", NULL, CHECKIT, X(4), (APTR)M_SKIP5 },
	{ NM_ITEM, (STRPTR)"Refresh", NULL, 0, 0, NULL },
	{ NM_SUB, (STRPTR)"Every frame", NULL, CHECKIT, X(0), (APTR)M_REF1 },
	{ NM_SUB, (STRPTR)"1 frame in 2", NULL, CHECKIT, X(1), (APTR)M_REF2 },
	{ NM_SUB, (STRPTR)"1 frame in 3", NULL, CHECKIT, X(2), (APTR)M_REF3 },
	{ NM_SUB, (STRPTR)"1 frame in 4", NULL, CHECKIT, X(3), (APTR)M_REF4 },
	{ NM_TITLE, (STRPTR)"Controls", NULL, 0, 0, NULL },
	{ NM_ITEM, (STRPTR)"Keyboard as joystick", NULL, 0, 0, NULL },
	{ NM_SUB, (STRPTR)"Off", NULL, CHECKIT, X(0), (APTR)M_JOY0 },
	{ NM_SUB, (STRPTR)"Cursors + Z / X", NULL, CHECKIT, X(1), (APTR)M_JOY1 },
	{ NM_SUB, (STRPTR)"Q A O P + Space / M", NULL, CHECKIT, X(2), (APTR)M_JOY2 },
	{ NM_ITEM, (STRPTR)"Pad in port 2", NULL, 0, 0, NULL },
	{ NM_SUB, (STRPTR)"Normal joystick / 2-button pad", NULL, CHECKIT, X(0), (APTR)M_PAD0 },
	{ NM_SUB, (STRPTR)"CD32 pad", NULL, CHECKIT, X(1), (APTR)M_PAD1 },
	{ NM_END, NULL, NULL, 0, 0, NULL }
};
#undef X

/* Everything is laid out in the window interior: OX/OY are its offsets from
 * the window corner (the borders and title bar), set before the gadgets. */
static int OX, OY;

static void text_at(int x, int y, int pen, const char *t)
{
	struct RastPort *rp = mwin->RPort;
	SetAPen(rp, pen); SetBPen(rp, P_BG); SetDrMd(rp, JAM2);
	Move(rp, OX + x, OY + y); Text(rp, (STRPTR)t, strlen(t));
}

/* a group: an etched (ridge) frame with its title on the top line, as the
 * OS 3.2 prefs editors draw them */
static void box(int x, int y, int w, int h, const char *title)
{
	DrawBevelBox(mwin->RPort, OX + x, OY + y, w, h, GT_VisualInfo, (ULONG)vi, GTBB_FrameType, BBFT_RIDGE, TAG_DONE);
	if (title) text_at(x + 10, y + 3, P_BLACK, title);
}

/* a label right-aligned to the gadget that starts at x */
static void label(int x, int y, const char *t)
{
	text_at(x - 6 - TextLength(mwin->RPort, (STRPTR)t, strlen(t)), y, P_BLACK, t);
}

static void status(const char *t)
{
	struct RastPort *rp = mwin->RPort;
	int w;
	text_at(20, 225, P_BLACK, t);
	w = TextLength(rp, (STRPTR)t, strlen(t));
	if (w < 596) { SetAPen(rp, P_BG); RectFill(rp, OX + 20 + w, OY + 218, OX + 624, OY + 227); }
}

static void logo(int x, int y)
{
	struct RastPort *rp = mwin->RPort;
	int i;
	x += OX; y += OY;
	SetAPen(rp, P_BLACK);
	RectFill(rp, x, y - 1, x + 299, y + 48);   /* meets the bevel: rows y-2 and y+49 */
	for (i = 0; i < Arnold_LogoRuns; i++) {
		const unsigned short *r = Arnold_Logo[i];
		SetAPen(rp, r[0]);
		RectFill(rp, x + 2 + r[1], y + 2 + r[2], x + 2 + r[1] + r[3] - 1, y + 2 + r[2]);
	}
	DrawBevelBox(rp, x - 2, y - 2, 304, 52, GT_VisualInfo, (ULONG)vi, GTBB_Recessed, TRUE, TAG_DONE);
}

static int skip_index(int v) { int i; for (i = 4; i > 0; i--) if (v >= skip_values[i]) return i; return 0; }
static int refresh_index(int v) { return v > 1 ? (v > 4 ? 3 : v - 1) : 0; }

/* the read-only summary of what the menus hold (3 lines, padded so a shorter
 * value wipes a longer one) */
static void draw_settings(struct SysMenu *m)
{
	char l[48];
	sprintf(l, "%-34.34s", ""); text_at(352, 147, P_BLACK, l); text_at(352, 164, P_BLACK, l); text_at(352, 181, P_BLACK, l);
	sprintf(l, "Sound: %-8s Screen: %s", m->sound ? "Paula" : "Off", m->hires ? "Hires" : "Lores");
	text_at(352, 147, P_BLACK, l);
	sprintf(l, "Skip: %-9s Refresh: %s", skip_labels[skip_index(m->skip)], refresh_labels[refresh_index(m->refresh)]);
	text_at(352, 164, P_BLACK, l);
	sprintf(l, "Joykeys: %-12s Pad: %s", joykey_labels[m->joykeys >= 0 && m->joykeys <= 2 ? m->joykeys : 0], pad_labels[m->pad ? 1 : 0]);
	text_at(352, 181, P_BLACK, l);
}

/* ASL file requester; writes the full path into buf (SYS_PATH). 1 if chosen. */
static int ask_file(const char *title, char *buf)
{
	struct FileRequester *fr;
	char drawer[SYS_PATH], *p;
	int ok = 0;
	strcpy(drawer, buf);
	p = (char *)PathPart((STRPTR)drawer); *p = 0;
	fr = AllocAslRequestTags(ASL_FileRequest,
		ASLFR_TitleText, (ULONG)title, ASLFR_Window, (ULONG)mwin,
		ASLFR_InitialDrawer, (ULONG)drawer, ASLFR_InitialFile, (ULONG)FilePart((STRPTR)buf),
		ASLFR_RejectIcons, TRUE, TAG_DONE);
	if (!fr) return 0;
	if (AslRequest(fr, NULL) && fr->fr_File[0]) {
		strncpy(buf, fr->fr_Drawer, SYS_PATH - 1); buf[SYS_PATH - 1] = 0;
		if (AddPart((STRPTR)buf, fr->fr_File, SYS_PATH)) ok = 1;
	}
	FreeAslRequest(fr);
	return ok;
}

static struct Gadget *row(struct Gadget *g, struct NewGadget *ng, int y, const char *value, int idstr, int idload, int ideject)
{
	ng->ng_LeftEdge = OX + 120; ng->ng_TopEdge = OY + y; ng->ng_Width = 360; ng->ng_Height = 14;
	ng->ng_GadgetText = NULL; ng->ng_GadgetID = idstr; ng->ng_Flags = 0;
	g = gad[idstr] = CreateGadget(STRING_KIND, g, ng, GTST_String, (ULONG)value, GTST_MaxChars, SYS_PATH - 1, TAG_DONE);
	ng->ng_LeftEdge = OX + 488; ng->ng_Width = 64; ng->ng_GadgetText = (STRPTR)"Load..."; ng->ng_GadgetID = idload;
	g = gad[idload] = CreateGadget(BUTTON_KIND, g, ng, TAG_DONE);
	ng->ng_LeftEdge = OX + 558; ng->ng_Width = 60; ng->ng_GadgetText = (STRPTR)"Eject"; ng->ng_GadgetID = ideject;
	g = gad[ideject] = CreateGadget(BUTTON_KIND, g, ng, TAG_DONE);
	return g;
}

static struct Gadget *button(struct Gadget *g, struct NewGadget *ng, int x, int y, int w, int h, int id, const char *label, int disabled)
{
	ng->ng_LeftEdge = OX + x; ng->ng_TopEdge = OY + y; ng->ng_Width = w; ng->ng_Height = h;
	ng->ng_GadgetText = (STRPTR)label; ng->ng_GadgetID = id; ng->ng_Flags = 0;
	return gad[id] = CreateGadget(BUTTON_KIND, g, ng, GA_Disabled, disabled, TAG_DONE);
}

static void close_menu(void)
{
	if (mwin) { ClearMenuStrip(mwin); CloseWindow(mwin); mwin = NULL; }
	if (menustrip) { FreeMenus(menustrip); menustrip = NULL; }
	if (glist) { FreeGadgets(glist); glist = NULL; }
	if (vi) { FreeVisualInfo(vi); vi = NULL; }
	if (mscr) { CloseScreen(mscr); mscr = NULL; }
}

static void draw_frame(struct SysMenu *m)
{
	logo(14, 4);
	DrawBevelBox(mwin->RPort, OX + 330, OY + 2, 298, 52, GT_VisualInfo, (ULONG)vi, TAG_DONE);
	text_at(330 + (298 - 25 * 8) / 2, 16, P_BLACK, "Amstrad CPC Plus emulator");
	text_at(330 + (298 - 28 * 8) / 2, 28, P_BLACK, "Arnold core by Kevin Thacker");
	text_at(330 + (298 - 26 * 8) / 2, 40, P_BLACK, "Amiga port 2026, GPL-2.0");

	/* rows are 17 apart (14-high gadgets, 3 clear), labels sit on the gadget's
	 * text line (+9) */
	box(12, 58, 616, 64, " Media ");
	label(120, 79, "Cartridge:");
	label(120, 96, "Drive A:");
	label(120, 113, "Drive B:");

	box(12, 127, 322, 64, " Game ");
	box(340, 127, 288, 64, " Settings (see the menus) ");
	draw_settings(m);

	/* the status line sits in a recessed field */
	DrawBevelBox(mwin->RPort, OX + 12, OY + 216, 616, 14, GT_VisualInfo, (ULONG)vi, GTBB_Recessed, TRUE, TAG_DONE);
}

/* the checkmarks in the menus from the settings */
static void set_checks(struct SysMenu *m)
{
	int i;
	int on[] = { M_SOUND, M_HIRES, M_SKIP0 + skip_index(m->skip), M_REF1 + refresh_index(m->refresh),
	             M_JOY0 + (m->joykeys >= 0 && m->joykeys <= 2 ? m->joykeys : 0), M_PAD0 + (m->pad ? 1 : 0) };
	for (i = 0; nm[i].nm_Type != NM_END; i++) {
		int id = (int)(ULONG)nm[i].nm_UserData, k, want = 0;
		if (!(nm[i].nm_Flags & CHECKIT)) continue;
		for (k = 0; k < 6; k++) if (on[k] == id) want = 1;
		if (id == M_SOUND) want = m->sound ? 1 : 0;
		if (id == M_HIRES) want = m->hires ? 1 : 0;
		if (want) nm[i].nm_Flags |= CHECKED; else nm[i].nm_Flags &= ~CHECKED;
	}
}

static int open_menu(struct SysMenu *m)
{
	struct NewGadget ng;
	struct Gadget *g;

	/* pens 8-15: the logo picture's colours */
	{
		int i;
		for (i = 0; i < 8; i++) {
			menupal[1 + (8 + i) * 3] = Arnold_LogoPalette[i][0] * 0x01010101UL;
			menupal[2 + (8 + i) * 3] = Arnold_LogoPalette[i][1] * 0x01010101UL;
			menupal[3 + (8 + i) * 3] = Arnold_LogoPalette[i][2] * 0x01010101UL;
		}
	}
	mscr = OpenScreenTags(NULL, SA_Width, 640, SA_Height, 256, SA_Depth, 4,
		SA_DisplayID, PAL_MONITOR_ID | HIRES_KEY, SA_Font, (ULONG)&topaz8,
		SA_Title, (ULONG)"Arnold - Amstrad CPC Plus emulator.  (c) Kevin Thacker, GPL-2.0", SA_Pens, (ULONG)menupens,
		SA_ShowTitle, TRUE, SA_Colors32, (ULONG)menupal,
		/* NTSC (19 Oct 2026, John's A1200): without pal.monitor the 256-line menu
		 * shows only 200 lines; autoscroll lets the mouse slide the screen up to
		 * the bottom rows (the menu shortcuts work as well) */
		SA_AutoScroll, TRUE, TAG_DONE);
	if (!mscr) return 0;
	vi = GetVisualInfo(mscr, TAG_DONE);
	if (!vi) { close_menu(); return 0; }
	OX = mscr->WBorLeft;
	OY = mscr->WBorTop + mscr->Font->ta_YSize + 1;
	g = CreateContext(&glist);
	memset(&ng, 0, sizeof ng);
	ng.ng_TextAttr = &topaz8; ng.ng_VisualInfo = vi;
	g = row(g, &ng, 70, m->cart, G_CART, G_CARTLOAD, G_CARTEJECT);
	g = row(g, &ng, 87, m->diska, G_DISKA, G_DISKALOAD, G_DISKAEJECT);
	g = row(g, &ng, 104, m->diskb, G_DISKB, G_DISKBLOAD, G_DISKBEJECT);
	g = button(g, &ng, 20, 138, 148, 14, G_SAVEGAME, "Save for game", 0);
	g = button(g, &ng, 174, 138, 152, 14, G_FORGETGAME, "Forget for game", !m->game_prefs);
	g = button(g, &ng, 20, 164, 148, 14, G_KEYS, "Keyboard...", 0);
	g = button(g, &ng, 174, 164, 152, 14, G_SHOT, "Screenshot", !m->can_continue);
	g = button(g, &ng, 20, 196, 140, 15, G_RESET, "Reset", 0);
	g = button(g, &ng, 173, 196, 140, 15, G_POWER, "Power ON", 0);
	g = button(g, &ng, 326, 196, 140, 15, G_CONTINUE, "Continue", !m->can_continue);
	g = button(g, &ng, 479, 196, 140, 15, G_QUIT, "Exit", 0);
	if (!g) { close_menu(); return 0; }
	set_checks(m);
	menustrip = CreateMenus(nm, GTMN_FullMenu, TRUE, TAG_DONE);
	if (!menustrip || !LayoutMenus(menustrip, vi, GTMN_NewLookMenus, TRUE, TAG_DONE)) { close_menu(); return 0; }
	mwin = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)mscr, WA_Left, 0, WA_Top, mscr->BarHeight + 1,
		WA_Width, 640, WA_Height, 256 - mscr->BarHeight - 1,
		WA_Title, (ULONG)"Arnold for Amiga v0.5.2  (Amstrad CPC Plus / GX4000)  built " __DATE__ " " __TIME__,
		WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_DragBar, FALSE,
		WA_Activate, TRUE, WA_NewLookMenus, TRUE,
		WA_Gadgets, (ULONG)glist, WA_SimpleRefresh, TRUE,
		WA_IDCMP, IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_RAWKEY | IDCMP_MENUPICK | IDCMP_EXTENDEDMOUSE | BUTTONIDCMP | STRINGIDCMP,
		TAG_DONE);
	if (!mwin) { close_menu(); return 0; }
	SetMenuStrip(mwin, menustrip);
	draw_frame(m);
	GT_RefreshWindow(mwin, NULL);
	{
		char line[SYS_PATH + 32];
		sprintf(line, "%s  Prefs: %s%s", m->status[0] ? m->status : (m->can_continue ? "Emulation paused: Help returns here, Continue resumes." : "Ready."),
			m->prefs[0] ? m->prefs : "(none)", m->game_prefs ? "  (game settings in use)" : "");
		m->status[0] = 0;
		status(line);
	}
	return 1;
}

static void take_strings(struct SysMenu *m)
{
	strncpy(m->cart, (char *)((struct StringInfo *)gad[G_CART]->SpecialInfo)->Buffer, SYS_PATH - 1);
	strncpy(m->diska, (char *)((struct StringInfo *)gad[G_DISKA]->SpecialInfo)->Buffer, SYS_PATH - 1);
	strncpy(m->diskb, (char *)((struct StringInfo *)gad[G_DISKB]->SpecialInfo)->Buffer, SYS_PATH - 1);
}

static void set_string(int id, const char *v) { GT_SetGadgetAttrs(gad[id], mwin, NULL, GTST_String, (ULONG)v, TAG_DONE); }

static void redraw_after_picture(struct SysMenu *m)
{
	/* the picture changed the pens and the window contents: put both back */
	LoadRGB32(&mscr->ViewPort, (ULONG *)menupal);
	SetAPen(mwin->RPort, P_BG);
	RectFill(mwin->RPort, OX, OY, mwin->Width - mwin->BorderRight - 1, mwin->Height - mwin->BorderBottom - 1);
	RefreshWindowFrame(mwin);
	draw_frame(m);
	RefreshGList(glist, mwin, NULL, -1);
	GT_RefreshWindow(mwin, NULL);
}

/* one chosen menu item; returns a SYS_MENU_ result to leave with, or -1 */
static int menu_pick(struct SysMenu *m, struct MenuItem *item)
{
	int id = (int)(ULONG)GTMENUITEM_USERDATA(item);
	int checked = (item->Flags & CHECKED) != 0;
	switch (id) {
	case M_LOADCART: take_strings(m); if (ask_file("Insert cartridge (.cpr)", m->cart)) set_string(G_CART, m->cart); break;
	case M_LOADA: take_strings(m); if (ask_file("Disk for drive A (.dsk)", m->diska)) set_string(G_DISKA, m->diska); break;
	case M_LOADB: take_strings(m); if (ask_file("Disk for drive B (.dsk)", m->diskb)) set_string(G_DISKB, m->diskb); break;
	case M_EJECTALL: set_string(G_CART, ""); set_string(G_DISKA, ""); set_string(G_DISKB, ""); break;
	case M_KEYS: Sys_ShowPicture(mscr, "PROGDIR:Keyboard.iff"); redraw_after_picture(m); break;
	case M_SHOT: if (m->can_continue) return SYS_MENU_SHOT; break;
	case M_SAVE: return SYS_MENU_SAVE;
	case M_SAVEGAME: return SYS_MENU_SAVEGAME;
	case M_FORGETGAME: if (m->game_prefs) return SYS_MENU_FORGETGAME; break;
	case M_QUIT: return SYS_MENU_QUIT;
	case M_SOUND: m->sound = checked; break;
	case M_HIRES: m->hires = checked; break;
	case M_SKIP0: case M_SKIP1: case M_SKIP2: case M_SKIP3: case M_SKIP5: m->skip = skip_values[id - M_SKIP0]; break;
	case M_REF1: case M_REF2: case M_REF3: case M_REF4: m->refresh = id - M_REF1 + 1; break;
	case M_JOY0: case M_JOY1: case M_JOY2: m->joykeys = id - M_JOY0; break;
	case M_PAD0: case M_PAD1: m->pad = id - M_PAD0; break;
	}
	draw_settings(m);
	return -1;
}

int Sys_Menu(struct SysMenu *m)
{
	int result = SYS_MENU_QUIT, done = 0;
	struct IntuiMessage *msg;

	if (!GadToolsBase) GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 37);
	if (!AslBase) AslBase = OpenLibrary((STRPTR)"asl.library", 37);
	if (!GadToolsBase || !AslBase) return SYS_MENU_QUIT;
	if (!open_menu(m)) return SYS_MENU_QUIT;

	while (!done) {
		WaitPort(mwin->UserPort);
		while ((msg = GT_GetIMsg(mwin->UserPort))) {
			ULONG cls = msg->Class; UWORD code = msg->Code;
			struct Gadget *gd = (struct Gadget *)msg->IAddress;
			GT_ReplyIMsg(msg);
			if (cls == IDCMP_CLOSEWINDOW) { result = SYS_MENU_QUIT; done = 1; }
			else if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(mwin); draw_frame(m); GT_EndRefresh(mwin, TRUE); }
			else if (cls == IDCMP_MENUPICK) {
				UWORD sel = code;
				while (sel != MENUNULL && !done) {
					struct MenuItem *item = ItemAddress(menustrip, sel);
					int r;
					if (!item) break;
					r = menu_pick(m, item);
					if (r >= 0) { result = r; done = 1; }
					sel = item->NextSelect;
				}
			}
			else if (cls == IDCMP_RAWKEY) {
				if (Sys_MenuKeyLog) { if (!mkeylog) mkeylog = fopen("PROGDIR:menukeys.log", "w"); if (mkeylog) { fprintf(mkeylog, "%02x\n", code); fflush(mkeylog); } }
				/* Shortcuts (rawkeys): Help/C Continue, P Power ON, R Reset, Q Quit,
				 * S Save Prefs, K Keyboard picture, X Screenshot. */
				switch (code) {
				case 0x5f: case 0x33: if (m->can_continue) { result = SYS_MENU_CONTINUE; done = 1; } break;
				case 0x19: result = SYS_MENU_POWER; done = 1; break;
				case 0x13: result = SYS_MENU_RESET; done = 1; break;
				case 0x10: result = SYS_MENU_QUIT; done = 1; break;
				case 0x21: result = SYS_MENU_SAVE; done = 1; break;
				case 0x32: if (m->can_continue) { result = SYS_MENU_SHOT; done = 1; } break;
				case 0x27: Sys_ShowPicture(mscr, "PROGDIR:Keyboard.iff"); redraw_after_picture(m); break;
				}
			}
			else if (cls == IDCMP_GADGETUP) {
				switch (gd->GadgetID) {
				case G_CARTLOAD: take_strings(m); if (ask_file("Insert cartridge (.cpr)", m->cart)) set_string(G_CART, m->cart); break;
				case G_DISKALOAD: take_strings(m); if (ask_file("Disk for drive A (.dsk)", m->diska)) set_string(G_DISKA, m->diska); break;
				case G_DISKBLOAD: take_strings(m); if (ask_file("Disk for drive B (.dsk)", m->diskb)) set_string(G_DISKB, m->diskb); break;
				case G_CARTEJECT: set_string(G_CART, ""); break;
				case G_DISKAEJECT: set_string(G_DISKA, ""); break;
				case G_DISKBEJECT: set_string(G_DISKB, ""); break;
				case G_POWER: result = SYS_MENU_POWER; done = 1; break;
				case G_CONTINUE: if (m->can_continue) { result = SYS_MENU_CONTINUE; done = 1; } break;
				case G_RESET: result = SYS_MENU_RESET; done = 1; break;
				case G_QUIT: result = SYS_MENU_QUIT; done = 1; break;
				case G_SAVEGAME: result = SYS_MENU_SAVEGAME; done = 1; break;
				case G_SHOT: result = SYS_MENU_SHOT; done = 1; break;
				case G_KEYS: Sys_ShowPicture(mscr, "PROGDIR:Keyboard.iff"); redraw_after_picture(m); break;
				case G_FORGETGAME: result = SYS_MENU_FORGETGAME; done = 1; break;
				}
			}
		}
	}
	take_strings(m);
	close_menu();
	return result;
}

void Sys_MenuError(const char *text)
{
	struct EasyStruct es = { sizeof(struct EasyStruct), 0, (STRPTR)"Arnold", (STRPTR)"%s", (STRPTR)"OK" };
	Sys_Log("error: %s", text);
	EasyRequest(NULL, &es, NULL, (ULONG)text);
}
