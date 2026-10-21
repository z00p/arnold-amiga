/* Arnold on AmigaOS: the menu screen (GadTools), styled after the OS 3.2 prefs
 * editors. 17 Oct 2026 (John): the settings live in pull-down menus (Options,
 * Controls, Display) and the window keeps what changes from game to game.
 * 19 Oct 2026 (John's A1200 in NTSC): reworked to fit a 200-line screen
 * without scrolling: header with the logo and a Settings group of editable
 * cycle gadgets (Model, Video mode, Game speed, as in AmiMSX), the Media
 * group, a one-line summary of the other settings, the Power ON / Reset /
 * Continue / Exit row and the status line. The per-game buttons, Keyboard and
 * Screenshot are in the Project menu. The screen opens in the chosen video
 * mode at that mode's nominal height (PAL 256, NTSC 200); the layout is the
 * same on both. Help in the emulation brings it up; Continue goes back.
 * OS side only: no Arnold headers (see amiga_sys.h). */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <graphics/modeid.h>
#include <graphics/displayinfo.h>
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
       G_POWER, G_RESET, G_CONTINUE, G_QUIT, G_MODEL, G_VIDEO, G_SPEED, G_COUNT };

/* menu item ids (nm_UserData) */
enum { M_NONE, M_LOADCART, M_LOADA, M_LOADB, M_EJECTALL, M_KEYS, M_SHOT, M_SAVE, M_SAVEGAME, M_FORGETGAME, M_QUIT,
       M_SOUND, M_HIRES, M_SKIP0, M_SKIP1, M_SKIP2, M_SKIP3, M_SKIP5, M_REF1, M_REF2, M_REF3, M_REF4,
       M_JOY0, M_JOY1, M_JOY2, M_PAD0, M_PAD1, M_SPEED50, M_SPEEDDISP,
       M_VIDAUTO, M_VIDPAL, M_VIDNTSC, M_TALL, M_MODEL464, M_MODEL664, M_MODEL6128, M_MODELPLUS, M_MODELGX };
#define MENU_REOPEN (-2)   /* menu_pick: the video mode changed, open the screen again */

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
static char wintitle[96];
static const int skip_values[] = { 0, 1, 2, 3, 5 };
static const char *skip_short[] = { "off", "1", "2", "3", "5" };
static const char *refresh_short[] = { "all", "1/2", "1/3", "1/4" };
static const char *joykey_short[] = { "off", "cursors", "QAOP" };
static const char *pad_short[] = { "normal", "CD32" };
/* the cycle gadgets' choices (NULL-terminated for GadTools) */
static STRPTR model_labels[] = { (STRPTR)"CPC 464", (STRPTR)"CPC 664", (STRPTR)"CPC 6128", (STRPTR)"CPC 6128 Plus", (STRPTR)"GX4000", NULL };
static STRPTR video_labels[] = { (STRPTR)"Auto (as Workbench)", (STRPTR)"PAL", (STRPTR)"NTSC", NULL };
static STRPTR speed_labels[] = { (STRPTR)"50 Hz (correct)", (STRPTR)"Follow display", NULL };

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
	{ NM_ITEM, (STRPTR)"CPC model (on Power ON)", NULL, 0, 0, NULL },
	{ NM_SUB, (STRPTR)"CPC 464", NULL, CHECKIT, X(0), (APTR)M_MODEL464 },
	{ NM_SUB, (STRPTR)"CPC 664", NULL, CHECKIT, X(1), (APTR)M_MODEL664 },
	{ NM_SUB, (STRPTR)"CPC 6128", NULL, CHECKIT, X(2), (APTR)M_MODEL6128 },
	{ NM_SUB, (STRPTR)"CPC 6128 Plus", NULL, CHECKIT, X(3), (APTR)M_MODELPLUS },
	{ NM_SUB, (STRPTR)"GX4000", NULL, CHECKIT, X(4), (APTR)M_MODELGX },
	{ NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
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
	{ NM_TITLE, (STRPTR)"Display", NULL, 0, 0, NULL },
	{ NM_ITEM, (STRPTR)"Video mode", NULL, 0, 0, NULL },
	{ NM_SUB, (STRPTR)"Auto (as Workbench)", NULL, CHECKIT, X(0), (APTR)M_VIDAUTO },
	{ NM_SUB, (STRPTR)"PAL", NULL, CHECKIT, X(1), (APTR)M_VIDPAL },
	{ NM_SUB, (STRPTR)"NTSC", NULL, CHECKIT, X(2), (APTR)M_VIDNTSC },
	{ NM_ITEM, (STRPTR)"Full height (all 272 lines)", NULL, CHECKIT | MENUTOGGLE, 0, (APTR)M_TALL },
	{ NM_ITEM, (STRPTR)"Game speed", NULL, 0, 0, NULL },
	{ NM_SUB, (STRPTR)"50 Hz (correct)", NULL, CHECKIT, X(0), (APTR)M_SPEED50 },
	{ NM_SUB, (STRPTR)"Follow display (NTSC: 20% fast)", NULL, CHECKIT, X(1), (APTR)M_SPEEDDISP },
	{ NM_END, NULL, NULL, 0, 0, NULL }
};
#undef X

/* Layout. Everything is in the window interior: OX/OY are its offsets from
 * the window corner (the borders and title bar), set before the gadgets.
 * Vertical positions come from the font height FH (topaz 8): gadgets are
 * GH = FH + 5 high, rows ROW = GH + 2 apart, and a group's first row starts
 * FH + 1 below its frame line (under the title). With topaz 8 the window
 * ends on line 188 of the screen, so it fits a 200-line NTSC screen. */
static int OX, OY;
static int FH, GH, ROW;
static int y_head, y_media, y_sum, y_buttons, y_status, inner_h;

static void layout(void)
{
	FH = topaz8.ta_YSize;
	GH = FH + 5;
	ROW = GH + 2;
	y_head = FH / 2;                         /* header: logo panel and the Settings group */
	y_media = y_head + 3 * ROW + FH + 4;     /* Media group frame */
	y_sum = y_media + 3 * ROW + FH + 6;      /* summary text line (top) */
	y_buttons = y_sum + FH + 4;              /* Power ON / Reset / Continue / Exit */
	y_status = y_buttons + GH + 1 + 4;       /* status field */
	inner_h = y_status + FH + 4 + 2;
}

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

/* a label right-aligned to the gadget that starts at x, on the gadget's text line */
static void label(int x, int gy, const char *t)
{
	text_at(x - 6 - TextLength(mwin->RPort, (STRPTR)t, strlen(t)), gy + (GH - FH) / 2 + mwin->RPort->TxBaseline, P_BLACK, t);
}

static void status(const char *t)
{
	struct RastPort *rp = mwin->RPort;
	int w, base = y_status + 2 + rp->TxBaseline;
	char line[80];
	strncpy(line, t, 76); line[76] = 0;      /* 76 characters fit the field */
	text_at(20, base, P_BLACK, line);
	w = TextLength(rp, (STRPTR)line, strlen(line));
	if (w < 600) { SetAPen(rp, P_BG); RectFill(rp, OX + 20 + w, OY + y_status + 2, OX + 620, OY + y_status + FH + 1); }
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
static int model_of(struct SysMenu *m) { return m->model >= 0 && m->model < SYS_MODELS ? m->model : SYS_MODEL_PLUS; }
static int video_of(struct SysMenu *m) { return m->video >= 0 && m->video <= 2 ? m->video : 0; }

/* the one-line summary of the settings that live in the pull-downs (padded
 * so a shorter line wipes a longer one) */
static void draw_settings(struct SysMenu *m)
{
	char l[96];
	int n;
	sprintf(l, "Sound %s  Screen %s  Skip %s  Refresh %s  Joykeys %s  Pad %s",
		m->sound ? "on" : "off", m->hires ? "hires" : "lores", skip_short[skip_index(m->skip)],
		refresh_short[refresh_index(m->refresh)], joykey_short[m->joykeys >= 0 && m->joykeys <= 2 ? m->joykeys : 0],
		pad_short[m->pad ? 1 : 0]);
	for (n = strlen(l); n < 76; n++) l[n] = ' ';
	l[n] = 0;
	text_at(20, y_sum + mwin->RPort->TxBaseline, P_BLACK, l);
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
	ng->ng_LeftEdge = OX + 120; ng->ng_TopEdge = OY + y; ng->ng_Width = 360; ng->ng_Height = GH;
	ng->ng_GadgetText = NULL; ng->ng_GadgetID = idstr; ng->ng_Flags = 0;
	g = gad[idstr] = CreateGadget(STRING_KIND, g, ng, GTST_String, (ULONG)value, GTST_MaxChars, SYS_PATH - 1, TAG_DONE);
	ng->ng_LeftEdge = OX + 488; ng->ng_Width = 64; ng->ng_GadgetText = (STRPTR)"Load..."; ng->ng_GadgetID = idload;
	g = gad[idload] = CreateGadget(BUTTON_KIND, g, ng, TAG_DONE);
	ng->ng_LeftEdge = OX + 558; ng->ng_Width = 62; ng->ng_GadgetText = (STRPTR)"Eject"; ng->ng_GadgetID = ideject;
	g = gad[ideject] = CreateGadget(BUTTON_KIND, g, ng, TAG_DONE);
	return g;
}

static struct Gadget *button(struct Gadget *g, struct NewGadget *ng, int x, int y, int w, int h, int id, const char *label, int disabled)
{
	ng->ng_LeftEdge = OX + x; ng->ng_TopEdge = OY + y; ng->ng_Width = w; ng->ng_Height = h;
	ng->ng_GadgetText = (STRPTR)label; ng->ng_GadgetID = id; ng->ng_Flags = 0;
	return gad[id] = CreateGadget(BUTTON_KIND, g, ng, GA_Disabled, disabled, TAG_DONE);
}

/* the Settings group's cycle gadgets: x of the gadgets, right of their labels */
#define SET_X 324
#define SET_GX (SET_X + 12 + 6 * 8 + 6)
static struct Gadget *cycle(struct Gadget *g, struct NewGadget *ng, int y, int id, STRPTR *labels, int active)
{
	ng->ng_LeftEdge = OX + SET_GX; ng->ng_TopEdge = OY + y; ng->ng_Width = 620 - SET_GX; ng->ng_Height = GH;
	ng->ng_GadgetText = NULL; ng->ng_GadgetID = id; ng->ng_Flags = 0;
	return gad[id] = CreateGadget(CYCLE_KIND, g, ng, GTCY_Labels, (ULONG)labels, GTCY_Active, active, TAG_DONE);
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
	int r0 = y_head + FH + 1;
	logo(14, y_head + 2);
	box(SET_X, y_head, 628 - SET_X, 3 * ROW + FH + 3, " Settings ");
	label(SET_GX, r0, "Model:");
	label(SET_GX, r0 + ROW, "Video:");
	label(SET_GX, r0 + 2 * ROW, "Speed:");

	box(12, y_media, 616, 3 * ROW + FH + 3, " Media ");
	label(120, y_media + FH + 1, "Cartridge:");
	label(120, y_media + FH + 1 + ROW, "Drive A:");
	label(120, y_media + FH + 1 + 2 * ROW, "Drive B:");
	draw_settings(m);

	/* the status line sits in a recessed field */
	DrawBevelBox(mwin->RPort, OX + 12, OY + y_status, 616, FH + 4, GT_VisualInfo, (ULONG)vi, GTBB_Recessed, TRUE, TAG_DONE);
}

/* the checkmarks in the menus from the settings */
static void set_checks(struct SysMenu *m)
{
	int i;
	int on[] = { M_SKIP0 + skip_index(m->skip), M_REF1 + refresh_index(m->refresh),
	             M_JOY0 + (m->joykeys >= 0 && m->joykeys <= 2 ? m->joykeys : 0), M_PAD0 + (m->pad ? 1 : 0),
	             M_SPEED50 + (m->speed ? 1 : 0), M_VIDAUTO + video_of(m), M_MODEL464 + model_of(m) };
	for (i = 0; nm[i].nm_Type != NM_END; i++) {
		int id = (int)(ULONG)nm[i].nm_UserData, k, want = 0;
		if (!(nm[i].nm_Flags & CHECKIT)) continue;
		for (k = 0; k < (int)(sizeof on / sizeof on[0]); k++) if (on[k] == id) want = 1;
		if (id == M_SOUND) want = m->sound ? 1 : 0;
		if (id == M_HIRES) want = m->hires ? 1 : 0;
		if (id == M_TALL) want = m->tall ? 1 : 0;
		if (want) nm[i].nm_Flags |= CHECKED; else nm[i].nm_Flags &= ~CHECKED;
	}
}

static int make_menus(struct SysMenu *m)
{
	set_checks(m);
	menustrip = CreateMenus(nm, GTMN_FullMenu, TRUE, TAG_DONE);
	return menustrip && LayoutMenus(menustrip, vi, GTMN_NewLookMenus, TRUE, TAG_DONE);
}

/* a cycle gadget changed a setting: the pull-downs' checkmarks follow */
static void refresh_menus(struct SysMenu *m)
{
	ClearMenuStrip(mwin);
	FreeMenus(menustrip); menustrip = NULL;
	if (make_menus(m)) SetMenuStrip(mwin, menustrip);
}

static void set_model(struct SysMenu *m, int model)
{
	char t[96];
	m->model = model;
	if (m->can_continue && m->running_model >= 0 && model != m->running_model)
		sprintf(t, "%s: takes effect on Power ON (running: %s).", (char *)model_labels[model], (char *)model_labels[m->running_model]);
	else if (model < SYS_MODEL_PLUS)
		sprintf(t, "%s: system ROMs, no cartridge. Power ON starts it.", (char *)model_labels[model]);
	else
		sprintf(t, "%s: takes effect on Power ON.", (char *)model_labels[model]);
	status(t);
}

static int open_menu(struct SysMenu *m)
{
	struct NewGadget ng;
	struct Gadget *g;
	struct DimensionInfo dims;
	ULONG modeid = Sys_ModeID(HIRES_KEY);
	int sh = 256, wh;

	layout();
	/* pens 8-15: the logo picture's colours */
	{
		int i;
		for (i = 0; i < 8; i++) {
			menupal[1 + (8 + i) * 3] = Arnold_LogoPalette[i][0] * 0x01010101UL;
			menupal[2 + (8 + i) * 3] = Arnold_LogoPalette[i][1] * 0x01010101UL;
			menupal[3 + (8 + i) * 3] = Arnold_LogoPalette[i][2] * 0x01010101UL;
		}
	}
	/* the mode's own height: 256 lines PAL, 200 NTSC */
	if (GetDisplayInfoData(NULL, (UBYTE *)&dims, sizeof dims, DTAG_DIMS, modeid) > 0)
		sh = dims.Nominal.MaxY - dims.Nominal.MinY + 1;
	if (sh < 200) sh = 200;
	if (sh > 256) sh = 256;
	mscr = OpenScreenTags(NULL, SA_Width, 640, SA_Height, sh, SA_Depth, 4,
		SA_DisplayID, modeid, SA_Font, (ULONG)&topaz8,
		SA_Title, (ULONG)"Arnold - Amstrad CPC / CPC Plus emulator.  (c) Kevin Thacker, GPL-2.0", SA_Pens, (ULONG)menupens,
		SA_ShowTitle, TRUE, SA_Colors32, (ULONG)menupal,
		/* a screen taller than the display (overscan settings) can still be slid */
		SA_AutoScroll, TRUE, TAG_DONE);
	if (!mscr) return 0;
	Sys_Log("menu: screen mode %08lx, %d lines", (unsigned long)modeid, sh);
	vi = GetVisualInfo(mscr, TAG_DONE);
	if (!vi) { close_menu(); return 0; }
	OX = mscr->WBorLeft;
	OY = mscr->WBorTop + mscr->Font->ta_YSize + 1;
	wh = OY + inner_h + mscr->WBorBottom;
	if (wh > sh - mscr->BarHeight - 1) wh = sh - mscr->BarHeight - 1;
	g = CreateContext(&glist);
	memset(&ng, 0, sizeof ng);
	ng.ng_TextAttr = &topaz8; ng.ng_VisualInfo = vi;
	g = cycle(g, &ng, y_head + FH + 1, G_MODEL, model_labels, model_of(m));
	g = cycle(g, &ng, y_head + FH + 1 + ROW, G_VIDEO, video_labels, video_of(m));
	g = cycle(g, &ng, y_head + FH + 1 + 2 * ROW, G_SPEED, speed_labels, m->speed ? 1 : 0);
	g = row(g, &ng, y_media + FH + 1, m->cart, G_CART, G_CARTLOAD, G_CARTEJECT);
	g = row(g, &ng, y_media + FH + 1 + ROW, m->diska, G_DISKA, G_DISKALOAD, G_DISKAEJECT);
	g = row(g, &ng, y_media + FH + 1 + 2 * ROW, m->diskb, G_DISKB, G_DISKBLOAD, G_DISKBEJECT);
	g = button(g, &ng, 12, y_buttons, 148, GH + 1, G_POWER, "Power ON", 0);
	g = button(g, &ng, 168, y_buttons, 148, GH + 1, G_RESET, "Reset", 0);
	g = button(g, &ng, 324, y_buttons, 148, GH + 1, G_CONTINUE, "Continue", !m->can_continue);
	g = button(g, &ng, 480, y_buttons, 148, GH + 1, G_QUIT, "Exit", 0);
	if (!g) { close_menu(); return 0; }
	if (!make_menus(m)) { close_menu(); return 0; }
	/* version, credit and build time (date and hh:mm) in the title bar */
	sprintf(wintitle, "Arnold v0.9 for Amiga - core: Kevin Thacker - built %.6s %.4s %.5s", __DATE__, __DATE__ + 7, __TIME__);
	mwin = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)mscr, WA_Left, 0, WA_Top, mscr->BarHeight + 1,
		WA_Width, 640, WA_Height, wh,
		WA_Title, (ULONG)wintitle,
		WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_DragBar, FALSE,
		WA_Activate, TRUE, WA_NewLookMenus, TRUE,
		WA_Gadgets, (ULONG)glist, WA_SimpleRefresh, TRUE,
		WA_IDCMP, IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_RAWKEY | IDCMP_MENUPICK | IDCMP_EXTENDEDMOUSE | BUTTONIDCMP | STRINGIDCMP | CYCLEIDCMP,
		TAG_DONE);
	if (!mwin) { close_menu(); return 0; }
	SetMenuStrip(mwin, menustrip);
	draw_frame(m);
	GT_RefreshWindow(mwin, NULL);
	{
		char line[SYS_PATH + 32];
		sprintf(line, "%s  Prefs: %s%s", m->status[0] ? m->status : (m->can_continue ? "Paused: Help returns here, Continue resumes." : "Ready."),
			m->prefs[0] ? (char *)FilePart((STRPTR)m->prefs) : "(none)", m->game_prefs ? " +game" : "");
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
static void set_cycle(int id, int v) { GT_SetGadgetAttrs(gad[id], mwin, NULL, GTCY_Active, v, TAG_DONE); }

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

/* one chosen menu item; returns a SYS_MENU_ result to leave with,
 * MENU_REOPEN, or -1 */
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
	case M_SHOT: if (m->can_continue) return SYS_MENU_SHOT; status("Screenshot: nothing is running yet."); break;
	case M_SAVE: return SYS_MENU_SAVE;
	case M_SAVEGAME: return SYS_MENU_SAVEGAME;
	case M_FORGETGAME: if (m->game_prefs) return SYS_MENU_FORGETGAME; status("This cartridge has no game settings."); break;
	case M_QUIT: return SYS_MENU_QUIT;
	case M_SOUND: m->sound = checked; break;
	case M_HIRES: m->hires = checked; break;
	case M_TALL: m->tall = checked; break;
	case M_SKIP0: case M_SKIP1: case M_SKIP2: case M_SKIP3: case M_SKIP5: m->skip = skip_values[id - M_SKIP0]; break;
	case M_REF1: case M_REF2: case M_REF3: case M_REF4: m->refresh = id - M_REF1 + 1; break;
	case M_JOY0: case M_JOY1: case M_JOY2: m->joykeys = id - M_JOY0; break;
	case M_PAD0: case M_PAD1: m->pad = id - M_PAD0; break;
	case M_SPEED50: case M_SPEEDDISP: m->speed = id - M_SPEED50; set_cycle(G_SPEED, m->speed); break;
	case M_VIDAUTO: case M_VIDPAL: case M_VIDNTSC:
		if (m->video != id - M_VIDAUTO) { m->video = id - M_VIDAUTO; return MENU_REOPEN; }
		break;
	case M_MODEL464: case M_MODEL664: case M_MODEL6128: case M_MODELPLUS: case M_MODELGX:
		set_model(m, id - M_MODEL464); set_cycle(G_MODEL, m->model); break;
	}
	draw_settings(m);
	return -1;
}

void Sys_MenuClose(void) { close_menu(); }

int Sys_Menu(struct SysMenu *m)
{
	int result = SYS_MENU_QUIT, done, reopen;
	struct IntuiMessage *msg;

	if (!GadToolsBase) GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 37);
	if (!AslBase) AslBase = OpenLibrary((STRPTR)"asl.library", 37);
	if (!GadToolsBase || !AslBase) return SYS_MENU_QUIT;
	close_menu();   /* one left open by the last call (e.g. a Power ON that failed) */

	/* a new video mode closes the screen and opens it again in that mode */
	do {
		Sys_VideoMode = video_of(m);
		if (!open_menu(m)) return SYS_MENU_QUIT;
		done = reopen = 0;
		while (!done) {
			WaitPort(mwin->UserPort);
			while (!done && (msg = GT_GetIMsg(mwin->UserPort))) {
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
						if (r == MENU_REOPEN) { reopen = 1; done = 1; }
						else if (r >= 0) { result = r; done = 1; }
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
					case G_MODEL: set_model(m, code < SYS_MODELS ? code : SYS_MODEL_PLUS); refresh_menus(m); break;
					case G_SPEED:
						m->speed = code ? 1 : 0; refresh_menus(m);
						status(m->speed ? "Game speed: follows the display (60 Hz: 20% fast)." : "Game speed: the CPC's 50 Hz on any display.");
						break;
					case G_VIDEO: if (code <= 2 && code != video_of(m)) { m->video = code; reopen = 1; done = 1; } break;
					}
				}
			}
		}
		take_strings(m);
		/* 19 Oct 2026 (John): the menu screen stays open until the host has opened
		 * the emulation screen in front of it (Sys_MenuClose), so Power ON or
		 * Continue no longer flashes the Workbench for a frame or two */
		if (reopen) close_menu();
		if (reopen) sprintf(m->status, "Video mode: %s.", (char *)video_labels[video_of(m)]);
	} while (reopen);
	return result;
}

void Sys_MenuError(const char *text)
{
	struct EasyStruct es = { sizeof(struct EasyStruct), 0, (STRPTR)"Arnold", (STRPTR)"%s", (STRPTR)"OK" };
	Sys_Log("error: %s", text);
	EasyRequest(NULL, &es, NULL, (ULONG)text);
}
