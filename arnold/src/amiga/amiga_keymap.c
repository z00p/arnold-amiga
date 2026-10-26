/* Arnold on AmigaOS: the key map editor (24 Oct 2026, John: "key mapping presets
 * with a simple UI to handle key mapping for a game").
 * A small GadTools window on the menu screen: the list of "Amiga key -> CPC key"
 * pairs, Add / Remove / Clear, the list of CPC keys, OK and Cancel. Add waits for
 * an Amiga key press, then a click in the CPC list completes the pair. OK writes
 * the map back as the remap= text (the same text the prefs files hold).
 * OS side only: no Arnold headers (see amiga_sys.h). */
#include <exec/types.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>
#include <clib/alib_protos.h>   /* NewList (amiga.lib) */
#include <stdio.h>
#include <string.h>
#include "amiga_sys.h"
#include "keymaplogic.h"

extern struct Library *GadToolsBase;   /* opened by amiga_menu.c */

enum { K_PAIRS, K_ADD, K_REMOVE, K_CLEAR, K_CPC, K_PRESET, K_PROMPT, K_OK, K_CANCEL, K_COUNT };
#define MAXCPC 96
#define IW 520            /* interior width */
#define LV_Y 14
#define LV_H 84           /* ten rows of topaz 8 plus the frame */
#define RAW_HELP 0x5f
#define PR_Y (LV_Y + LV_H + 4)   /* preset row */
#define KeyPresetMax 16

static struct TextAttr topaz8 = { (STRPTR)"topaz.font", 8, 0, 0 };
static struct Gadget *gad[K_COUNT], *glist;
static struct Window *win;
static struct List plist, clist;
static struct Node pnode[KM_MAXPAIRS], cnode[MAXCPC];
static char ptxt[KM_MAXPAIRS][32];
static struct KeyPair pr[KM_MAXPAIRS];
static int npr, sel, capturing, OX, OY;
static char pending[12], prompt[80];
static STRPTR plabels[KeyPresetMax + 2];   /* the preset cycle: every preset, then "Custom" */
static int npreset, npreset_active;

static void say(const char *t)
{
	strncpy(prompt, t, sizeof prompt - 1); prompt[sizeof prompt - 1] = 0;
	GT_SetGadgetAttrs(gad[K_PROMPT], win, NULL, GTTX_Text, (ULONG)prompt, TAG_DONE);
}

static void sync_preset(void);
static void rebuild(void)
{
	int i;
	char a[16], c[16];
	GT_SetGadgetAttrs(gad[K_PAIRS], win, NULL, GTLV_Labels, (ULONG)~0, TAG_DONE);
	NewList(&plist);
	for (i = 0; i < npr; i++) {
		KeyMap_ShowName(pr[i].a, a); KeyMap_ShowName(pr[i].c, c);
		sprintf(ptxt[i], "%-9s -> %s", a, c);
		pnode[i].ln_Name = ptxt[i]; pnode[i].ln_Type = 0; pnode[i].ln_Pri = 0;
		AddTail(&plist, &pnode[i]);
	}
	sel = -1;
	GT_SetGadgetAttrs(gad[K_PAIRS], win, NULL, GTLV_Labels, (ULONG)&plist, GTLV_Selected, (ULONG)~0, TAG_DONE);
	sync_preset();
}

/* the preset cycle shows the preset the list equals, else "Custom" */
static void sync_preset(void)
{
	char cur[KM_STR];
	int k;
	KeyMap_Format(pr, npr, cur, sizeof cur);
	k = KeyMap_PresetOf(cur);
	GT_SetGadgetAttrs(gad[K_PRESET], win, NULL, GTCY_Active, k >= 0 ? k : npreset, TAG_DONE);
}

static void text_at(int x, int y, const char *t)
{
	struct RastPort *rp = win->RPort;
	SetAPen(rp, 1); SetBPen(rp, 0); SetDrMd(rp, JAM2);
	Move(rp, OX + x, OY + y + rp->TxBaseline); Text(rp, (STRPTR)t, strlen(t));
}

static void draw(void)
{
	text_at(8, 3, "Amiga key -> CPC key");
	text_at(308, 3, "CPC key (pick after Add)");
	text_at(8, PR_Y + 3, "Preset:");
}

static struct Gadget *mk(struct Gadget *g, struct NewGadget *ng, int kind, int id, int x, int y, int w, int h, const char *label, ULONG tag1, ULONG v1, ULONG tag2, ULONG v2)
{
	ng->ng_LeftEdge = OX + x; ng->ng_TopEdge = OY + y; ng->ng_Width = w; ng->ng_Height = h;
	ng->ng_GadgetText = (STRPTR)label; ng->ng_GadgetID = id; ng->ng_Flags = 0;
	return gad[id] = CreateGadget(kind, g, ng, tag1, v1, tag2, v2, TAG_DONE);
}

/* remap: in = the current map, out = the edited map when OK. Returns 1 if OK was chosen. */
int Sys_KeyMapEdit(void *screen, void *parent, void *visinfo, char *remap)
{
	struct Screen *scr = (struct Screen *)screen;
	struct Window *pw = (struct Window *)parent;
	struct NewGadget ng;
	struct Gadget *g;
	struct Requester req;
	struct IntuiMessage *msg;
	int i, ok = 0, done = 0, w, h, blocked = 0;
	const int GH = 13;

	if (!GadToolsBase || !scr || win) return 0;
	npr = KeyMap_Parse(remap, pr, KM_MAXPAIRS);
	sel = -1; capturing = 0; pending[0] = 0;
	NewList(&clist);
	for (i = 0; i < MAXCPC && KeyMap_CpcNames[i]; i++) {
		cnode[i].ln_Name = (char *)KeyMap_CpcNames[i]; cnode[i].ln_Type = 0; cnode[i].ln_Pri = 0;
		AddTail(&clist, &cnode[i]);
	}
	NewList(&plist);
	npreset = KeyMap_NPresets();
	if (npreset > KeyPresetMax) npreset = KeyPresetMax;
	for (i = 0; i < npreset; i++) plabels[i] = (STRPTR)KeyMap_Presets[i].label;
	plabels[npreset] = (STRPTR)"Custom"; plabels[npreset + 1] = NULL;
	{ char cur[KM_STR]; i = KeyMap_PresetOf(remap); (void)cur; if (i < 0 || i >= npreset) i = npreset; npreset_active = i; }
	OX = scr->WBorLeft;
	OY = scr->WBorTop + scr->Font->ta_YSize + 1;
	memset(&ng, 0, sizeof ng);
	ng.ng_TextAttr = &topaz8; ng.ng_VisualInfo = visinfo;
	g = CreateContext(&glist);
	g = mk(g, &ng, LISTVIEW_KIND, K_PAIRS, 8, LV_Y, 208, LV_H, NULL, GTLV_Labels, (ULONG)&plist, GTLV_ShowSelected, 0);
	g = mk(g, &ng, BUTTON_KIND, K_ADD, 224, LV_Y, 76, GH, "Add...", TAG_IGNORE, 0, TAG_IGNORE, 0);
	g = mk(g, &ng, BUTTON_KIND, K_REMOVE, 224, LV_Y + 15, 76, GH, "Remove", TAG_IGNORE, 0, TAG_IGNORE, 0);
	g = mk(g, &ng, BUTTON_KIND, K_CLEAR, 224, LV_Y + 30, 76, GH, "Clear", TAG_IGNORE, 0, TAG_IGNORE, 0);
	g = mk(g, &ng, LISTVIEW_KIND, K_CPC, 308, LV_Y, 204, LV_H, NULL, GTLV_Labels, (ULONG)&clist, GTLV_ShowSelected, 0);
	g = mk(g, &ng, CYCLE_KIND, K_PRESET, 84, PR_Y, 330, GH, NULL, GTCY_Labels, (ULONG)plabels, GTCY_Active, (ULONG)npreset_active);
	g = mk(g, &ng, TEXT_KIND, K_PROMPT, 8, PR_Y + GH + 3, IW - 16, GH, NULL, GTTX_Border, TRUE, GTTX_CopyText, TRUE);
	g = mk(g, &ng, BUTTON_KIND, K_OK, 8, PR_Y + 2 * GH + 8, 120, GH + 1, "OK", TAG_IGNORE, 0, TAG_IGNORE, 0);
	g = mk(g, &ng, BUTTON_KIND, K_CANCEL, IW - 128, PR_Y + 2 * GH + 8, 120, GH + 1, "Cancel", TAG_IGNORE, 0, TAG_IGNORE, 0);
	if (!g) { FreeGadgets(glist); glist = NULL; return 0; }
	w = OX + IW + scr->WBorRight;
	h = OY + PR_Y + 2 * GH + 8 + GH + 1 + 5 + scr->WBorBottom;
	if (pw) {   /* modal: a requester blocks the menu window's gadgets and menus */
		InitRequester(&req);
		blocked = Request(&req, pw);
	}
	win = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)scr, WA_Left, (640 - w) / 2, WA_Top, scr->BarHeight + 1 + 2,
		WA_Width, w, WA_Height, h, WA_Title, (ULONG)"Edit key map",
		WA_CloseGadget, TRUE, WA_DragBar, TRUE, WA_DepthGadget, FALSE, WA_Activate, TRUE, WA_SimpleRefresh, TRUE,
		WA_Gadgets, (ULONG)glist,
		WA_IDCMP, IDCMP_GADGETUP | IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | IDCMP_RAWKEY | LISTVIEWIDCMP | BUTTONIDCMP | TEXTIDCMP,
		TAG_DONE);
	if (!win) {
		if (blocked) EndRequest(&req, pw);
		FreeGadgets(glist); glist = NULL;
		return 0;
	}
	GT_RefreshWindow(win, NULL);
	draw();
	rebuild();
	say(npr ? "Add: press an Amiga key, then pick its CPC key." : "No keys remapped. Add: press an Amiga key, then pick its CPC key.");

	while (!done) {
		WaitPort(win->UserPort);
		while (!done && (msg = GT_GetIMsg(win->UserPort))) {
			ULONG cls = msg->Class; UWORD code = msg->Code, qual = msg->Qualifier;
			struct Gadget *gd = (struct Gadget *)msg->IAddress;
			GT_ReplyIMsg(msg);
			if (cls == IDCMP_CLOSEWINDOW) done = 1;
			else if (cls == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(win); draw(); GT_EndRefresh(win, TRUE); }
			else if (cls == IDCMP_RAWKEY) {
				char nb[12], a[16], t[80];
				const char *nm;
				if ((code & 0x80) || (qual & IEQUALIFIER_REPEAT)) continue;
				if (!capturing) {   /* keys: A Add, P next preset, Return OK, Esc Cancel */
					if (code == 0x44) { ok = 1; done = 1; }
					else if (code == 0x45) done = 1;
					else if (code == 0x20) { capturing = 1; pending[0] = 0; say("Press the Amiga key to remap now (Help cancels)..."); }
					else if (code == 0x19) {
						char cur[KM_STR];
						KeyMap_Format(pr, npr, cur, sizeof cur);
						i = KeyMap_PresetOf(cur); i = i < 0 ? 0 : (i + 1) % npreset;
						npr = KeyMap_Parse(KeyMap_Presets[i].remap, pr, KM_MAXPAIRS); rebuild(); say(KeyMap_Presets[i].label);
					}
					continue;
				}
				if (code == RAW_HELP) { capturing = 0; say("Cancelled."); continue; }
				nm = Host_AmigaKeyName(code, nb);
				if (!nm) { say("That key can't be remapped. Press another (Help cancels)."); continue; }
				strcpy(pending, nm); capturing = 0;
				KeyMap_ShowName(pending, a);
				sprintf(t, "Amiga key %s: now pick its CPC key in the list.", a);
				say(t);
			}
			else if (cls == IDCMP_GADGETUP) {
				switch (gd->GadgetID) {
				case K_ADD: capturing = 1; pending[0] = 0; say("Press the Amiga key to remap now (Help cancels)..."); break;
				case K_PAIRS: sel = code; break;
				case K_PRESET:
					if (code < npreset) { npr = KeyMap_Parse(KeyMap_Presets[code].remap, pr, KM_MAXPAIRS); rebuild(); say(KeyMap_Presets[code].label); }
					break;
				case K_REMOVE:
					if (sel >= 0 && sel < npr) {
						for (i = sel; i < npr - 1; i++) pr[i] = pr[i + 1];
						npr--; rebuild(); say("Removed.");
					} else say("Select a pair in the list first.");
					break;
				case K_CLEAR: npr = 0; rebuild(); say("Cleared."); break;
				case K_CPC:
					if (pending[0] && code < MAXCPC && KeyMap_CpcNames[code]) {
						struct KeyPair keep[KM_MAXPAIRS];
						int kn = npr, n2;
						memcpy(keep, pr, sizeof pr);
						n2 = KeyMap_Set(pr, npr, KM_MAXPAIRS, pending, KeyMap_CpcNames[code]);
						if (n2 < 0 || !KeyMap_FitsString(pr, n2)) { memcpy(pr, keep, sizeof pr); npr = kn; say("The key map is full."); }
						else {
							char a[16], c[16], t[80];
							npr = n2; rebuild();
							KeyMap_ShowName(pending, a); KeyMap_ShowName(KeyMap_CpcNames[code], c);
							sprintf(t, "Added %s -> %s. Add another, or OK.", a, c); say(t);
							pending[0] = 0;
						}
					} else say("Choose Add first, then press an Amiga key.");
					break;
				case K_OK: ok = 1; done = 1; break;
				case K_CANCEL: done = 1; break;
				}
			}
		}
	}
	if (ok) KeyMap_Format(pr, npr, remap, KM_STR);
	CloseWindow(win); win = NULL;
	if (blocked) EndRequest(&req, pw);
	FreeGadgets(glist); glist = NULL;
	return ok;
}
