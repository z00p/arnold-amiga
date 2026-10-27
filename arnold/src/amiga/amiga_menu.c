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
#include "keymaplogic.h"

struct Library *GadToolsBase, *AslBase;
int Sys_MenuKeyLog;   /* host sets it with KEYLOG: raw keys seen by the menu go to PROGDIR:menukeys.log */
static FILE *mkeylog;

enum { G_CART, G_CARTLOAD, G_CARTEJECT, G_DISKA, G_DISKALOAD, G_DISKAEJECT,
       G_DISKB, G_DISKBLOAD, G_DISKBEJECT,
       G_TAPE, G_TAPELOAD, G_TAPEEJECT, G_TAPEREW,
       G_POWER, G_RESET, G_CONTINUE, G_QUIT, G_MODEL, G_VIDEO, G_SPEED, G_COUNT };

/* menu item ids (nm_UserData) */
enum { M_NONE, M_LOADCART, M_LOADA, M_LOADB, M_EJECTALL, M_KEYS, M_SHOT, M_SAVE, M_SAVEGAME, M_FORGETGAME, M_QUIT,
       M_SOUND, M_HIRES, M_SKIP0, M_SKIP1, M_SKIP2, M_SKIP3, M_SKIP5, M_REF1, M_REF2, M_REF3, M_REF4,
       M_JOY0, M_JOY1, M_JOY2, M_JOY3, M_PAD0, M_PAD1, M_SPEED50, M_SPEEDDISP,
       M_VIDAUTO, M_VIDPAL, M_VIDNTSC, M_TALL, M_WIDE, M_DISKWRITE, M_MODEL464, M_MODEL664, M_MODEL6128, M_MODELPLUS, M_MODELGX,
       M_LOADTAPE, M_REWIND, M_FASTTAPE, M_TAPESOUND, M_KEYEDIT, M_KEYMAPSUB, M_RTG,
       M_SCAN0, M_SCAN1, M_SCAN2, M_SCAN3, M_SCAN4, M_SCAN5, M_SCAN6, M_PRESET0 };   /* M_PRESET0 last: the presets follow it */
#define MAXPRESETS 12   /* menu entries for KeyMap_Presets (keymaplogic.c) */
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
static const char *joykey_short[] = { "off", "cursors", "QAOP", "cursors X" };
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
	{ NM_ITEM, (STRPTR)"Load tape...", (STRPTR)"T", 0, 0, (APTR)M_LOADTAPE },
	{ NM_ITEM, (STRPTR)"Rewind tape", (STRPTR)"W", 0, 0, (APTR)M_REWIND },
	{ NM_ITEM, (STRPTR)"Eject all", NULL, 0, 0, (APTR)M_EJECTALL },
	{ NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
	{ NM_ITEM, (STRPTR)"Keyboard map...", (STRPTR)"K", 0, 0, (APTR)M_KEYS },
	{ NM_ITEM, (STRPTR)"Screenshot", (STRPTR)"X", 0, 0, (APTR)M_SHOT },
	{ NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
	{ NM_ITEM, (STRPTR)"Save Prefs", (STRPTR)"S", 0, 0, (APTR)M_SAVE },
	{ NM_ITEM, (STRPTR)"Save for game", (STRPTR)"G", 0, 0, (APTR)M_SAVEGAME },
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
	{ NM_ITEM, (STRPTR)"Write disks back", NULL, CHECKIT | MENUTOGGLE, 0, (APTR)M_DISKWRITE },
	{ NM_ITEM, (STRPTR)"Fast tape loading", NULL, CHECKIT | MENUTOGGLE, 0, (APTR)M_FASTTAPE },
	{ NM_ITEM, (STRPTR)"Tape loading sound", NULL, CHECKIT | MENUTOGGLE, 0, (APTR)M_TAPESOUND },
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
	{ NM_SUB, (STRPTR)"Cursors + Z fire / X fire 2", NULL, CHECKIT, X(1), (APTR)M_JOY1 },
	{ NM_SUB, (STRPTR)"Q A O P + Space fire / M fire 2", NULL, CHECKIT, X(2), (APTR)M_JOY2 },
	{ NM_SUB, (STRPTR)"Cursors + X fire / Z fire 2", NULL, CHECKIT, X(3), (APTR)M_JOY3 },
	{ NM_ITEM, (STRPTR)"Pad in port 2", NULL, 0, 0, NULL },
	{ NM_SUB, (STRPTR)"Normal joystick / 2-button pad", NULL, CHECKIT, X(0), (APTR)M_PAD0 },
	{ NM_SUB, (STRPTR)"CD32 pad", NULL, CHECKIT, X(1), (APTR)M_PAD1 },
	{ NM_ITEM, (STRPTR)"Key map", NULL, 0, 0, (APTR)M_KEYMAPSUB },   /* sub-items: the presets, added by build_menu_table */
	{ NM_ITEM, (STRPTR)"Edit key map...", (STRPTR)"E", 0, 0, (APTR)M_KEYEDIT },
	{ NM_TITLE, (STRPTR)"Display", NULL, 0, 0, NULL },
	{ NM_ITEM, (STRPTR)"Video mode", NULL, 0, 0, NULL },
	{ NM_SUB, (STRPTR)"Auto (as Workbench)", NULL, CHECKIT, X(0), (APTR)M_VIDAUTO },
	{ NM_SUB, (STRPTR)"PAL", NULL, CHECKIT, X(1), (APTR)M_VIDPAL },
	{ NM_SUB, (STRPTR)"NTSC", NULL, CHECKIT, X(2), (APTR)M_VIDNTSC },
	{ NM_ITEM, (STRPTR)"Full height (all 272 lines)", NULL, CHECKIT | MENUTOGGLE, 0, (APTR)M_TALL },
	{ NM_ITEM, (STRPTR)"Wide (all 384 columns)", NULL, CHECKIT | MENUTOGGLE, 0, (APTR)M_WIDE },
	{ NM_ITEM, (STRPTR)"RTG screen", (STRPTR)"D", CHECKIT | MENUTOGGLE, 0, (APTR)M_RTG },
	{ NM_ITEM, (STRPTR)"RTG scanlines", NULL, 0, 0, NULL },   /* 1.2: SYS_SCAN_* in menu order; the key N in the menu picks the next one */
	{ NM_SUB, (STRPTR)"Off (sharp 1x picture)", NULL, CHECKIT, X(0), (APTR)M_SCAN0 },
	{ NM_SUB, (STRPTR)"Hard 25%", NULL, CHECKIT, X(1), (APTR)M_SCAN1 },
	{ NM_SUB, (STRPTR)"Hard 50%", NULL, CHECKIT, X(2), (APTR)M_SCAN2 },
	{ NM_SUB, (STRPTR)"Hard 75%", NULL, CHECKIT, X(3), (APTR)M_SCAN3 },
	{ NM_SUB, (STRPTR)"Blended light", NULL, CHECKIT, X(4), (APTR)M_SCAN4 },
	{ NM_SUB, (STRPTR)"Blended", NULL, CHECKIT, X(5), (APTR)M_SCAN5 },
	{ NM_SUB, (STRPTR)"Blended strong", NULL, CHECKIT, X(6), (APTR)M_SCAN6 },
	{ NM_ITEM, (STRPTR)"Game speed", NULL, 0, 0, NULL },
	{ NM_SUB, (STRPTR)"50 Hz (correct)", NULL, CHECKIT, X(0), (APTR)M_SPEED50 },
	{ NM_SUB, (STRPTR)"Follow display (NTSC: 20% fast)", NULL, CHECKIT, X(1), (APTR)M_SPEEDDISP },
	{ NM_END, NULL, NULL, 0, 0, NULL }
};
#undef X
/* the table CreateMenus gets: nm with the Key map presets (keymaplogic.c) expanded under "Key map" */
static struct NewMenu nmx[sizeof nm / sizeof nm[0] + MAXPRESETS];
static void build_menu_table(void)
{
	int i, j = 0, k, np = KeyMap_NPresets();
	if (np > MAXPRESETS) np = MAXPRESETS;
	for (i = 0; i < (int)(sizeof nm / sizeof nm[0]); i++) {
		nmx[j++] = nm[i];
		if ((int)(ULONG)nm[i].nm_UserData == M_KEYMAPSUB && nm[i].nm_Type == NM_ITEM)
			for (k = 0; k < np; k++) {
				struct NewMenu *e = &nmx[j++];
				e->nm_Type = NM_SUB; e->nm_Label = (STRPTR)KeyMap_Presets[k].label; e->nm_CommKey = NULL;
				e->nm_Flags = CHECKIT; e->nm_MutualExclude = ~(1UL << k); e->nm_UserData = (APTR)(M_PRESET0 + k);
			}
	}
}

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
	y_sum = y_media + 4 * ROW + FH + 4;      /* summary text line (top); the Media group has 4 rows since the Tape row (24 Oct 2026) */
	y_buttons = y_sum + FH + 3;              /* Power ON / Reset / Continue / Exit */
	y_status = y_buttons + GH + 1 + 3;       /* status field (gaps 1 line tighter each: ends on screen line 199) */
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
	char l[96], mid[96], comp[96], out[96];
	int n, jk = m->joykeys >= 0 && m->joykeys <= 3 ? m->joykeys : 0, rf = refresh_index(m->refresh);
	static const char *const scan_short[SYS_SCANS] = { "RTG", "RTG+25", "RTG+50", "RTG+75", "RTG+bl-", "RTG+bl", "RTG+bl+" };
	const char *scrn = m->rtg ? (m->hires ? "RTG-hi" : scan_short[m->scan > 0 && m->scan < SYS_SCANS ? m->scan : 0]) : (m->hires ? "hires" : "lores");
	sprintf(l, "Sound %s  Screen %s  Skip %s  Refresh %s  Joykeys %s  Pad %s",
		m->sound ? "on" : "off", scrn, skip_short[skip_index(m->skip)],
		refresh_short[rf], joykey_short[jk], pad_short[m->pad ? 1 : 0]);
	sprintf(mid, "Sound %s  Screen %s  Skip %s  Ref %s  Joy %s  Pad %s",
		m->sound ? "on" : "off", scrn, skip_short[skip_index(m->skip)],
		refresh_short[rf], joykey_short[jk], pad_short[m->pad ? 1 : 0]);
	sprintf(comp, "Snd %s Scr %s Skip %s Ref %s Joy %s Pad %s",
		m->sound ? "on" : "off", scrn, skip_short[skip_index(m->skip)],
		refresh_short[rf], joykey_short[jk], pad_short[m->pad ? 1 : 0]);
	KeyMap_FitLine(out, 74, l, mid, comp, m->remap);   /* 74 chars: the window ends at 8 px per char from x 20 */   /* adds "Keys: RShift=M" (or "Keys: 3 remapped") */
	for (n = strlen(out); n < 74; n++) out[n] = ' ';
	out[n] = 0;
	text_at(20, y_sum + mwin->RPort->TxBaseline, P_BLACK, out);
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
		ASLFR_TitleText, (ULONG)title, ASLFR_Window, (ULONG)mwin, ASLFR_Screen, (ULONG)mscr,   /* on the menu's own screen (RTG or AGA) */
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
	ng->ng_LeftEdge = OX + 120; ng->ng_TopEdge = OY + y; ng->ng_Width = idstr == G_TAPE ? 290 : 360; ng->ng_Height = GH;
	ng->ng_GadgetText = NULL; ng->ng_GadgetID = idstr; ng->ng_Flags = 0;
	g = gad[idstr] = CreateGadget(STRING_KIND, g, ng, GTST_String, (ULONG)value, GTST_MaxChars, SYS_PATH - 1, TAG_DONE);
	ng->ng_LeftEdge = OX + 488; ng->ng_Width = 64; ng->ng_GadgetText = (STRPTR)"Load..."; ng->ng_GadgetID = idload;
	g = gad[idload] = CreateGadget(BUTTON_KIND, g, ng, TAG_DONE);
	ng->ng_LeftEdge = OX + 558; ng->ng_Width = 62; ng->ng_GadgetText = (STRPTR)"Eject"; ng->ng_GadgetID = ideject;
	g = gad[ideject] = CreateGadget(BUTTON_KIND, g, ng, TAG_DONE);
	return g;
}

/* the Tape row: a shorter string gadget and a Rewind button before Load... / Eject */
static struct Gadget *tape_row(struct Gadget *g, struct NewGadget *ng, int y, const char *value)
{
	g = row(g, ng, y, value, G_TAPE, G_TAPELOAD, G_TAPEEJECT);
	if (!g) return NULL;
	ng->ng_LeftEdge = OX + 418; ng->ng_TopEdge = OY + y; ng->ng_Width = 64; ng->ng_Height = GH;
	ng->ng_GadgetText = (STRPTR)"Rewind"; ng->ng_GadgetID = G_TAPEREW; ng->ng_Flags = 0;
	return gad[G_TAPEREW] = CreateGadget(BUTTON_KIND, g, ng, TAG_DONE);
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

/* A menu screen with everything on it. The next menu is opened BEFORE the old one is closed
 * (RTG, 26 Oct 2026): nothing else (Workbench, an AGA screen) shows in between, whether the
 * next menu is a Save Prefs, an option that reopens it, or the one after Help. */
struct MenuHold { struct Screen *scr; struct Window *win; struct Menu *strip; struct Gadget *gl; APTR vi; int rtg; };
static int menu_rtg;                 /* the open menu screen is an RTG screen */
int Sys_MenuOnRtg(void) { return mscr && menu_rtg; }
void (*Sys_MenuOpenedHook)(void);

static void menu_detach(struct MenuHold *h)
{
	h->scr = mscr; h->win = mwin; h->strip = menustrip; h->gl = glist; h->vi = vi; h->rtg = menu_rtg;
	mscr = NULL; mwin = NULL; menustrip = NULL; glist = NULL; vi = NULL; menu_rtg = 0;
}

static void menu_free(struct MenuHold *h)
{
	if (h->win) { ClearMenuStrip(h->win); CloseWindow(h->win); h->win = NULL; }
	if (h->strip) { FreeMenus(h->strip); h->strip = NULL; }
	if (h->gl) { FreeGadgets(h->gl); h->gl = NULL; }
	if (h->vi) { FreeVisualInfo(h->vi); h->vi = NULL; }
	if (h->scr) { CloseScreen(h->scr); h->scr = NULL; Sys_Log("menu: closed (%s screen)", h->rtg ? "RTG" : "AGA"); }
}

static void close_menu(void)
{
	struct MenuHold h;
	menu_detach(&h);
	menu_free(&h);
}

static void draw_frame(struct SysMenu *m)
{
	int r0 = y_head + FH + 1;
	logo(14, y_head + 2);
	box(SET_X, y_head, 628 - SET_X, 3 * ROW + FH + 3, " Settings ");
	label(SET_GX, r0, "Model:");
	label(SET_GX, r0 + ROW, "Video:");
	label(SET_GX, r0 + 2 * ROW, "Speed:");

	box(12, y_media, 616, 4 * ROW + FH + 3, " Media ");
	label(120, y_media + FH + 1, "Cartridge:");
	label(120, y_media + FH + 1 + ROW, "Drive A:");
	label(120, y_media + FH + 1 + 2 * ROW, "Drive B:");
	label(120, y_media + FH + 1 + 3 * ROW, "Tape:");
	draw_settings(m);

	/* the status line sits in a recessed field */
	DrawBevelBox(mwin->RPort, OX + 12, OY + y_status, 616, FH + 4, GT_VisualInfo, (ULONG)vi, GTBB_Recessed, TRUE, TAG_DONE);
}

/* the checkmarks in the menus from the settings */
static void set_checks(struct SysMenu *m)
{
	int i;
	int on[] = { M_SKIP0 + skip_index(m->skip), M_REF1 + refresh_index(m->refresh),
	             M_JOY0 + (m->joykeys >= 0 && m->joykeys <= 3 ? m->joykeys : 0), M_PAD0 + (m->pad ? 1 : 0),
	             M_SPEED50 + (m->speed ? 1 : 0), M_VIDAUTO + video_of(m), M_MODEL464 + model_of(m),
	             M_SCAN0 + (m->scan > 0 && m->scan < SYS_SCANS ? m->scan : 0) };
	for (i = 0; nmx[i].nm_Type != NM_END; i++) {
		int id = (int)(ULONG)nmx[i].nm_UserData, k, want = 0;
		if (!(nmx[i].nm_Flags & CHECKIT)) continue;
		for (k = 0; k < (int)(sizeof on / sizeof on[0]); k++) if (on[k] == id) want = 1;
		if (id >= M_PRESET0 && id < M_PRESET0 + MAXPRESETS) want = KeyMap_PresetOf(m->remap) == id - M_PRESET0;
		if (id == M_SOUND) want = m->sound ? 1 : 0;
		if (id == M_HIRES) want = m->hires ? 1 : 0;
		if (id == M_TALL) want = m->tall ? 1 : 0;
		if (id == M_WIDE) want = m->wide ? 1 : 0;
		if (id == M_RTG) want = m->rtg ? 1 : 0;
		if (id == M_DISKWRITE) want = m->diskwrite ? 1 : 0;
		if (id == M_FASTTAPE) want = m->fasttape ? 1 : 0;
		if (id == M_TAPESOUND) want = m->tapesound ? 1 : 0;
		if (want) nmx[i].nm_Flags |= CHECKED; else nmx[i].nm_Flags &= ~CHECKED;
	}
}

static int make_menus(struct SysMenu *m)
{
	build_menu_table();
	set_checks(m);
	menustrip = CreateMenus(nmx, GTMN_FullMenu, TRUE, TAG_DONE);
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
	int sh = 256, wh, wl = 0, wt;

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
	/* RTG screen on (26 Oct 2026): the menu on an 8-bit RTG screen too (the smallest mode of
	 * 640x256 or more), so the picture stays on the RTG output (HDMI) between play and menu;
	 * the 640-wide window is centred on it. No RTG mode, or refused: the AGA menu screen. */
	mscr = NULL; menu_rtg = 0;
	if (m->rtg && !Sys_RtgDead) {
		int rw, rh;
		ULONG rid = Sys_RtgMenuMode(&rw, &rh);
		if (!rid) { Sys_RtgDead = 1; if (!m->status[0]) strcpy(m->status, "RTG screen: no RTG board or 8-bit mode, AGA used."); }
		if (rid) {
			mscr = OpenScreenTags(NULL, SA_Width, rw, SA_Height, rh, SA_Depth, 8,
				SA_DisplayID, rid, SA_Font, (ULONG)&topaz8,
				SA_Title, (ULONG)"Arnold - Amstrad CPC / CPC Plus emulator.  (c) Kevin Thacker, GPL-2.0", SA_Pens, (ULONG)menupens,
				SA_ShowTitle, TRUE, SA_Colors32, (ULONG)menupal, TAG_DONE);
			if (mscr) { sh = rh; wl = (rw - 640) / 2; menu_rtg = 1; Sys_Log("menu: RTG screen mode %08lx, %dx%d", (unsigned long)rid, rw, rh); }
			else {
				Sys_Log("menu: RTG screen mode %08lx refused, AGA menu screen", (unsigned long)rid);
				Sys_RtgDead = 1; if (!m->status[0]) strcpy(m->status, "RTG screen: refused, AGA used.");
			}
		}
	}
	if (!mscr) {
	mscr = OpenScreenTags(NULL, SA_Width, 640, SA_Height, sh, SA_Depth, 4,
		SA_DisplayID, modeid, SA_Font, (ULONG)&topaz8,
		SA_Title, (ULONG)"Arnold - Amstrad CPC / CPC Plus emulator.  (c) Kevin Thacker, GPL-2.0", SA_Pens, (ULONG)menupens,
		SA_ShowTitle, TRUE, SA_Colors32, (ULONG)menupal,
		/* a screen taller than the display (overscan settings) can still be slid */
		SA_AutoScroll, TRUE, TAG_DONE);
	if (!mscr) return 0;
	Sys_Log("menu: screen mode %08lx, %d lines", (unsigned long)modeid, sh);
	}
	vi = GetVisualInfo(mscr, TAG_DONE);
	if (!vi) { close_menu(); return 0; }
	OX = mscr->WBorLeft;
	OY = mscr->WBorTop + mscr->Font->ta_YSize + 1;
	wh = OY + inner_h + mscr->WBorBottom;
	if (wh > sh - mscr->BarHeight - 1) wh = sh - mscr->BarHeight - 1;
	wt = mscr->BarHeight + 1;
	if (sh > 256 && sh - wt - wh > 1) wt += (sh - wt - wh) / 2;   /* RTG screen (taller than any AGA one): centred */
	g = CreateContext(&glist);
	memset(&ng, 0, sizeof ng);
	ng.ng_TextAttr = &topaz8; ng.ng_VisualInfo = vi;
	g = cycle(g, &ng, y_head + FH + 1, G_MODEL, model_labels, model_of(m));
	g = cycle(g, &ng, y_head + FH + 1 + ROW, G_VIDEO, video_labels, video_of(m));
	g = cycle(g, &ng, y_head + FH + 1 + 2 * ROW, G_SPEED, speed_labels, m->speed ? 1 : 0);
	g = row(g, &ng, y_media + FH + 1, m->cart, G_CART, G_CARTLOAD, G_CARTEJECT);
	g = row(g, &ng, y_media + FH + 1 + ROW, m->diska, G_DISKA, G_DISKALOAD, G_DISKAEJECT);
	g = row(g, &ng, y_media + FH + 1 + 2 * ROW, m->diskb, G_DISKB, G_DISKBLOAD, G_DISKBEJECT);
	g = tape_row(g, &ng, y_media + FH + 1 + 3 * ROW, m->tape);
	g = button(g, &ng, 12, y_buttons, 148, GH + 1, G_POWER, "Power ON", 0);
	g = button(g, &ng, 168, y_buttons, 148, GH + 1, G_RESET, "Reset", 0);
	g = button(g, &ng, 324, y_buttons, 148, GH + 1, G_CONTINUE, "Continue", !m->can_continue);
	g = button(g, &ng, 480, y_buttons, 148, GH + 1, G_QUIT, "Exit", 0);
	if (!g) { close_menu(); return 0; }
	if (!make_menus(m)) { close_menu(); return 0; }
	/* version, credit and build time (date and hh:mm) in the title bar */
	sprintf(wintitle, "Arnold v1.3 for Amiga - core: Kevin Thacker - built %.6s %.4s %.5s", __DATE__, __DATE__ + 7, __TIME__);
	mwin = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)mscr, WA_Left, wl, WA_Top, wt,
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
		snprintf(line, sizeof line, "%s  %sPrefs: %s%s", m->status[0] ? m->status : (m->can_continue ? "Paused: Help returns here, Continue resumes." : "Ready."),
			m->tapepos, m->prefs[0] ? (char *)FilePart((STRPTR)m->prefs) : "(none)", m->game_prefs == 2 ? " +db" : m->game_prefs ? " +game" : "");
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
	strncpy(m->tape, (char *)((struct StringInfo *)gad[G_TAPE]->SpecialInfo)->Buffer, SYS_PATH - 1);
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

/* Rewind (button or Project menu): done by the host on Continue / Reset;
 * Power ON always starts the tape from the beginning */
static void rewind_tape(struct SysMenu *m)
{
	take_strings(m);
	if (!m->tape[0]) { status("Rewind: no tape in the deck."); return; }
	m->tape_rewind = 1;
	status("Tape: rewound to the start (on Continue or Reset).");
}

/* one chosen menu item; returns a SYS_MENU_ result to leave with,
 * MENU_REOPEN, or -1 */
static int menus_dirty;
static void edit_keymap(struct SysMenu *m)
{
	if (Sys_KeyMapEdit(mscr, mwin, vi, m->remap)) {
		status(m->remap[0] ? "Key map applied. Save for game keeps it." : "Key map cleared.");
		menus_dirty = 1;   /* the preset checkmarks follow */
	}
	ActivateWindow(mwin);   /* the editor was the active window: give the keyboard back */
	draw_settings(m);
}
/* RTG screen ticked or unticked: the menu (and, from Continue or Power ON, the picture) switches at once */
static void rtg_set(struct SysMenu *m, int on)
{
	m->rtg = on;
	if (on) Sys_RtgDead = 0;   /* ticked again: try once more */
	strcpy(m->status, !on ? "RTG screen off: the AGA screen from here on."
		: Sys_RtgAvailable() ? "RTG screen on: the menu and the picture stay on it."
		: "RTG screen: no RTG board or 8-bit mode found, AGA stays.");
}

/* RTG scanlines chosen (Display menu, or N in the menu for the next one): used from the next Continue / Power ON */
static void scan_set(struct SysMenu *m, int k)
{
	static const char *const what[SYS_SCANS] = { "off (the sharp 1x picture)", "hard 25%", "hard 50%", "hard 75%",
		"blended light", "blended", "blended strong" };
	char t[96];
	m->scan = k >= 0 && k < SYS_SCANS ? k : 0;
	sprintf(t, "RTG scanlines: %s.%s", what[m->scan], m->rtg ? " Continue shows it." : " Tick RTG screen to see them.");
	status(t);
}

static int menu_pick(struct SysMenu *m, struct MenuItem *item)
{
	int id = (int)(ULONG)GTMENUITEM_USERDATA(item);
	int checked = (item->Flags & CHECKED) != 0;
	if (id >= M_PRESET0 && id < M_PRESET0 + MAXPRESETS) {
		char t[96];
		const struct KeyPreset *kp = &KeyMap_Presets[id - M_PRESET0];
		strcpy(m->remap, kp->remap);
		sprintf(t, "Key map: %s.%s", kp->label, kp->remap[0] ? " Save for game keeps it." : "");
		status(t);
		draw_settings(m);
		return -1;
	}
	switch (id) {
	case M_KEYEDIT: edit_keymap(m); break;
	case M_LOADCART: take_strings(m); if (ask_file("Insert cartridge (.cpr)", m->cart)) set_string(G_CART, m->cart); break;
	case M_LOADA: take_strings(m); if (ask_file("Disk for drive A (.dsk)", m->diska)) set_string(G_DISKA, m->diska); break;
	case M_LOADB: take_strings(m); if (ask_file("Disk for drive B (.dsk)", m->diskb)) set_string(G_DISKB, m->diskb); break;
	case M_LOADTAPE: take_strings(m); if (ask_file("Insert tape (.cdt)", m->tape)) set_string(G_TAPE, m->tape); break;
	case M_REWIND: rewind_tape(m); break;
	case M_EJECTALL: set_string(G_CART, ""); set_string(G_DISKA, ""); set_string(G_DISKB, ""); set_string(G_TAPE, ""); break;
	case M_KEYS: Sys_ShowPicture(mscr, "PROGDIR:Keyboard.iff"); redraw_after_picture(m); break;
	case M_SHOT: if (m->can_continue) return SYS_MENU_SHOT; status("Screenshot: nothing is running yet."); break;
	case M_SAVE: return SYS_MENU_SAVE;
	case M_SAVEGAME: return SYS_MENU_SAVEGAME;
	case M_FORGETGAME:
		if (m->game_prefs == 2) { status("These settings come from Games.db: Save for game overrides them."); break; }
		if (m->game_prefs) return SYS_MENU_FORGETGAME;
		status("This cartridge has no game settings.");
		break;
	case M_QUIT: return SYS_MENU_QUIT;
	case M_SOUND: m->sound = checked; break;
	case M_HIRES: m->hires = checked; break;
	case M_TALL: m->tall = checked; break;
	case M_WIDE: m->wide = checked; break;
	case M_RTG: rtg_set(m, checked);
		return MENU_REOPEN;
	case M_SCAN0: case M_SCAN1: case M_SCAN2: case M_SCAN3: case M_SCAN4: case M_SCAN5: case M_SCAN6:
		scan_set(m, id - M_SCAN0);
		break;
	case M_DISKWRITE: m->diskwrite = checked;
		status(checked ? "Disks: changes are written back to the .dsk files." : "Disks: changes are kept until Power ON or Exit only.");
		break;
	case M_FASTTAPE: m->fasttape = checked;
		status(checked ? "Fast tape: loading runs flat out, picture and sound paused." : "Fast tape off: tapes load at real speed.");
		break;
	case M_TAPESOUND: m->tapesound = checked;
		status(checked ? "Tape sound: you hear the tape while it loads at real speed." : "Tape sound off.");
		break;
	case M_SKIP0: case M_SKIP1: case M_SKIP2: case M_SKIP3: case M_SKIP5: m->skip = skip_values[id - M_SKIP0]; break;
	case M_REF1: case M_REF2: case M_REF3: case M_REF4: m->refresh = id - M_REF1 + 1; break;
	case M_JOY0: case M_JOY1: case M_JOY2: case M_JOY3: m->joykeys = id - M_JOY0; break;
	case M_PAD0: case M_PAD1: m->pad = id - M_PAD0; break;
	case M_SPEED50: case M_SPEEDDISP: m->speed = id - M_SPEED50; set_cycle(G_SPEED, m->speed); break;
	case M_VIDAUTO: case M_VIDPAL: case M_VIDNTSC:
		if (m->video != id - M_VIDAUTO) { m->video = id - M_VIDAUTO; if (menu_rtg) { set_cycle(G_VIDEO, m->video); break; } return MENU_REOPEN; }
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
	struct MenuHold old;

	if (!GadToolsBase) GadToolsBase = OpenLibrary((STRPTR)"gadtools.library", 37);
	if (!AslBase) AslBase = OpenLibrary((STRPTR)"asl.library", 37);
	if (!GadToolsBase || !AslBase) return SYS_MENU_QUIT;
	menu_detach(&old);   /* one left open by the last call (a Power ON that failed, Save Prefs on RTG): closed once the new one is up */

	/* a new video mode closes the screen and opens it again in that mode */
	do {
		Sys_VideoMode = video_of(m);
		if (!open_menu(m)) { menu_free(&old); return SYS_MENU_QUIT; }
		menu_free(&old);
		if (Sys_MenuOpenedHook) { void (*h)(void) = Sys_MenuOpenedHook; Sys_MenuOpenedHook = NULL; h(); }
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
					if (menus_dirty) { menus_dirty = 0; if (!done) refresh_menus(m); }
				}
				else if (cls == IDCMP_RAWKEY) {
					if (Sys_MenuKeyLog) { if (!mkeylog) mkeylog = fopen("PROGDIR:menukeys.log", "w"); if (mkeylog) { fprintf(mkeylog, "%02x\n", code); fflush(mkeylog); } }
					/* Shortcuts (rawkeys): Help/C Continue, P Power ON, R Reset, Q Quit,
					 * S Save Prefs, G Save for game, D RTG screen on/off, N next RTG scanlines, L Load cartridge, E Edit key map, K Keyboard picture, X Screenshot. */
					switch (code) {
					case 0x5f: case 0x33: if (m->can_continue) { result = SYS_MENU_CONTINUE; done = 1; } break;
					case 0x19: result = SYS_MENU_POWER; done = 1; break;
					case 0x13: result = SYS_MENU_RESET; done = 1; break;
					case 0x10: result = SYS_MENU_QUIT; done = 1; break;
					case 0x21: result = SYS_MENU_SAVE; done = 1; break;
					case 0x32: if (m->can_continue) { result = SYS_MENU_SHOT; done = 1; } break;
					case 0x12: edit_keymap(m); if (menus_dirty) { menus_dirty = 0; refresh_menus(m); } break;
					case 0x24: result = SYS_MENU_SAVEGAME; done = 1; break;
					case 0x22: rtg_set(m, !m->rtg); reopen = 1; done = 1; break;
					case 0x36: scan_set(m, (m->scan + 1) % SYS_SCANS); refresh_menus(m); draw_settings(m); break;   /* N: next RTG scanlines */
					case 0x28: take_strings(m); if (ask_file("Insert cartridge (.cpr)", m->cart)) set_string(G_CART, m->cart); break;   /* L: Load cartridge */   /* D: the RTG screen (Display menu) */
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
					case G_TAPELOAD: take_strings(m); if (ask_file("Insert tape (.cdt)", m->tape)) set_string(G_TAPE, m->tape); break;
					case G_TAPEEJECT: set_string(G_TAPE, ""); break;
					case G_TAPEREW: rewind_tape(m); break;
					case G_POWER: result = SYS_MENU_POWER; done = 1; break;
					case G_CONTINUE: if (m->can_continue) { result = SYS_MENU_CONTINUE; done = 1; } break;
					case G_RESET: result = SYS_MENU_RESET; done = 1; break;
					case G_QUIT: result = SYS_MENU_QUIT; done = 1; break;
					case G_MODEL: set_model(m, code < SYS_MODELS ? code : SYS_MODEL_PLUS); refresh_menus(m); break;
					case G_SPEED:
						m->speed = code ? 1 : 0; refresh_menus(m);
						status(m->speed ? "Game speed: follows the display (60 Hz: 20% fast)." : "Game speed: the CPC's 50 Hz on any display.");
						break;
					case G_VIDEO: if (code <= 2 && code != video_of(m)) { m->video = code; if (!menu_rtg) { reopen = 1; done = 1; } } break;
					}
				}
			}
		}
		take_strings(m);
		/* 19 Oct 2026 (John): the menu screen stays open until the host has opened
		 * the emulation screen in front of it (Sys_MenuClose), so Power ON or
		 * Continue no longer flashes the Workbench for a frame or two */
		if (reopen) menu_detach(&old);   /* freed when the next one is open */
		if (reopen && !m->status[0]) sprintf(m->status, "Video mode: %s.", (char *)video_labels[video_of(m)]);
	} while (reopen);
	return result;
}

void Sys_MenuError(const char *text)
{
	struct EasyStruct es = { sizeof(struct EasyStruct), 0, (STRPTR)"Arnold", (STRPTR)"%s", (STRPTR)"OK" };
	Sys_Log("error: %s", text);
	EasyRequest(mwin, &es, NULL, (ULONG)text);   /* mwin: on the menu's own screen (RTG or AGA); NULL = the default public screen */
}
