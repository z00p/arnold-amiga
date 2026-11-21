/* Arnold on AmigaOS: Workbench start. With libnix, main() gets argc == 0 and
 * argv == the WBStartup message. We read the PREFS ToolType from the icon we
 * were started from (Arnold's own, or a preset project icon) and take a project icon
 * (a .cpr/.dsk/.sna double-clicked, or dropped on a preset) as the media.
 * The current directory is set to the icon's drawer, so relative names work. */
#include <exec/types.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/memory.h>
#include <string.h>
#include <stdlib.h>
#include "amiga_sys.h"
#ifndef NO_LUPA
#include <exec/execbase.h>
#include <dos/var.h>
#include <dos/dostags.h>
#include <stdio.h>
#endif

struct Library *IconBase;

/* The ToolTypes of an icon (PREFS, MENU, HIRES, NOSOUND, SKIP, REFRESH) into st.
 * Returns 1 if the icon was found. */
static int read_tooltypes(STRPTR name, struct SysStart *st)
{
	struct DiskObject *dobj = GetDiskObject(name);
	char *tt;
	if (!dobj) return 0;
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"PREFS");
	if (tt) strncpy(st->prefs, tt, SYS_PATH - 1);
	if (FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"MENU")) st->menu = 1;
	if (FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"HIRES")) st->hires = 1;
	if (FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"TALL")) st->tall = 1;
	if (FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"WIDE")) st->wide = 1;
	if (FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"NOSOUND")) st->nosound = 1;
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"SKIP");
	if (tt) st->skip = atoi(tt);
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"REFRESH");
	if (tt) st->refresh = atoi(tt);
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"JOYKEYS");
	if (tt) st->joykeys = atoi(tt);
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"REMAP");
	if (tt) strncpy(st->remap, tt, sizeof st->remap - 1);   /* REMAP=RSHIFT:M,... (key remap) */
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"PAD");
#ifndef NO_CD32KIT
	if (tt) st->pad = !strcmp(tt, "KTRL") || !strcmp(tt, "CD32+") || atoi(tt) == 2 ? 2 : !strcmp(tt, "CD32") || atoi(tt) == 1;   /* PAD=KTRL: the low clock */
#else
	if (tt) st->pad = !strcmp(tt, "CD32") || atoi(tt) == 1;
#endif
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"SPEED");
	if (tt) st->speed = Sys_ParseSpeed(tt);   /* SPEED=50 or SPEED=DISPLAY */
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"VIDEO");
	if (tt) st->video = Sys_ParseVideo(tt);   /* VIDEO=AUTO|PAL|NTSC */
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"MODEL");
	if (tt) st->model = Sys_ParseModel(tt);   /* MODEL=464|664|6128|PLUS|GX4000 */
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"DISKWRITE");
	if (tt) st->diskwrite = atoi(tt) ? 1 : 0;   /* DISKWRITE=0|1: write changed disks back */
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"FASTTAPE");
	if (tt) st->fasttape = atoi(tt) ? 1 : 0;   /* FASTTAPE=0|1: fast tape loading (24 Oct 2026) */
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"TAPESOUND");
	if (tt) st->tapesound = (!*tt || atoi(tt)) ? 1 : 0;   /* TAPESOUND=0|1 (25 Oct 2026; a bare TAPESOUND means on) */
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"AUTOTAPE");
	if (tt) st->autotape = (!*tt || atoi(tt)) ? 1 : 0;   /* AUTOTAPE=0|1: auto-start tapes (1.5; a bare AUTOTAPE means on) */
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"HDMISOUND");
	if (tt) st->hdmisound = (!*tt || atoi(tt)) ? 1 : 0;   /* HDMISOUND=0|1: on the RTG screen, sound through AHI */
#ifndef NO_USBPADS
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"USBPADS");
	if (tt) st->usbpads = (!*tt || atoi(tt)) ? 1 : 0;   /* USBPADS=0|1: lowlevel ports 2-3 (2.1.1; a bare USBPADS means on) */
#endif
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"RTGLOCK");
	if (tt && *tt) st->rtglock = atoi(tt);   /* RTGLOCK=0|1|2: frame lock to the HDMI blank (0 never, 1 with HDMI sound or none, 2 always; rtg17) */
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"TONE");
	if (tt) st->tone = Sys_ParseTone(tt);   /* TONE=BRIGHT|SOFT|WARM or 0-2: the sound's low-pass, Paula and HDMI (hdmifilt) */
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"RTG");
	if (tt) st->rtg = (!*tt || atoi(tt)) ? 1 : 0;   /* RTG=0|1, a bare RTG means on: the RTG screen (pilot, 26 Oct 2026) */
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"RTGVBL");
	if (tt) Sys_RtgVblank = (!*tt || atoi(tt)) ? 1 : 0;   /* RTGVBL=0|1: RTG frames written at the display's vertical blank (27 Oct 2026) */
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"SCANLINES");
	if (tt) st->scan = Sys_ParseScan(tt);   /* SCANLINES=OFF|25|50|75|BLENDLIGHT|BLEND|BLENDSTRONG (RTG screen, 1.2) */
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"TAPE");
	if (tt) strncpy(st->tape, tt, SYS_PATH - 1);   /* TAPE=<file.cdt> */
#ifndef NO_LUPA
	if (FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"ONEGAME")) Sys_LupaOneGame = 1;   /* Lupa: Help quits to Workbench */
	if (FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"AGA")) st->rtg = 0;               /* Lupa: the AGA screen */
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"PROFILE");
	if (tt) strncpy(Sys_LupaProfile, tt, sizeof Sys_LupaProfile - 1);   /* Lupa: AUTO|FAST|030 */
#endif
	FreeDiskObject(dobj);
	return 1;
}

/* Disk write-back (22 Oct 2026, cpc/diskimage/dskwrite.c): 0 if the file is
 * protected against writing or deleting, or its volume is write-protected. */
int Host_FileWritable(const char *name)
{
	struct FileInfoBlock *fib;
	struct InfoData *id;
	BPTR l = Lock((STRPTR)name, SHARED_LOCK);
	int ok = 1;
	if (!l) return 0;
	fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
	if (fib && Examine(l, fib) && (fib->fib_Protection & (FIBF_WRITE | FIBF_DELETE))) ok = 0;   /* RWED bits are set = not allowed */
	if (fib) FreeDosObject(DOS_FIB, fib);
	id = (struct InfoData *)AllocVec(sizeof *id, MEMF_PUBLIC);   /* longword aligned */
	if (id && Info(l, id) && id->id_DiskState == ID_WRITE_PROTECTED) ok = 0;
	if (id) FreeVec(id);
	UnLock(l);
	return ok;
}

/* N1 of the code review: on OFS/FFS of older Kickstarts a name is cut to 30
 * characters, so <name>.tmp/.bak/.old can be one file, or the image itself.
 * 1 if a and b both exist and are the same object. */
int Host_SameFile(const char *a, const char *b)
{
	BPTR la = Lock((STRPTR)a, SHARED_LOCK), lb;
	int same = 0;
	if (!la) return 0;
	lb = Lock((STRPTR)b, SHARED_LOCK);
	if (lb) {
		LONG r = SameLock(la, lb);
		if (r == LOCK_SAME) same = 1;
		else if (r == LOCK_SAME_VOLUME) {   /* a handler without SameLock support: compare the disk keys */
			struct FileInfoBlock *fa = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
			struct FileInfoBlock *fb = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
			if (fa && fb && Examine(la, fa) && Examine(lb, fb) && fa->fib_DiskKey && fa->fib_DiskKey == fb->fib_DiskKey) same = 1;
			if (fa) FreeDosObject(DOS_FIB, fa);
			if (fb) FreeDosObject(DOS_FIB, fb);
		}
		UnLock(lb);
	}
	UnLock(la);
	return same;
}
static int same_name(const char *a, const char *b)   /* AmigaDOS names: case-insensitive (ASCII) */
{
	for (; *a && *b; a++, b++) {
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z') x += 32;
		if (y >= 'A' && y <= 'Z') y += 32;
		if (x != y) return 0;
	}
	return *a == *b;
}
/* 0 if the file exists but is stored under another name than asked for (cut short). */
int Host_NameKept(const char *name)
{
	struct FileInfoBlock *fib;
	BPTR l = Lock((STRPTR)name, SHARED_LOCK);
	int ok = 1;
	if (!l) return 1;
	fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
	if (fib && Examine(l, fib) && !same_name((const char *)fib->fib_FileName, (const char *)FilePart((STRPTR)name))) ok = 0;
	if (fib) FreeDosObject(DOS_FIB, fib);
	UnLock(l);
	return ok;
}

int Sys_WBStart(int argc, char **argv, struct SysStart *st)
{
	struct WBStartup *wbs;
	struct WBArg *wa;
	memset(st, 0, sizeof *st);
	st->skip = -1;
	st->joykeys = -1; st->pad = -1; st->speed = -1; st->video = -1; st->model = -1; st->diskwrite = -1; st->fasttape = -1; st->tapesound = -1; st->rtg = -1; st->scan = -1; st->autotape = -1; st->hdmisound = -1; st->rtglock = -1; st->tone = -1;
#ifndef NO_USBPADS
	st->usbpads = -1;
#endif
	if (argc != 0) return 0;
	wbs = (struct WBStartup *)argv;
	if (!wbs || wbs->sm_NumArgs < 1) return 0;
	if (!IconBase) IconBase = OpenLibrary((STRPTR)"icon.library", 37);

	wa = wbs->sm_ArgList;
	if (wa[0].wa_Lock) CurrentDir(wa[0].wa_Lock);
	if (IconBase && wa[0].wa_Name) read_tooltypes(wa[0].wa_Name, st);
	/* A project icon. A preset (like AmiMSX's: default tool Arnold, PREFS=<its
	 * own data file>) gives its ToolTypes; the PREFS file is taken from the
	 * preset's drawer. Anything else (a .cpr/.dsk/.sna) is the media file. */
	if (wbs->sm_NumArgs > 1 && wa[1].wa_Name) {
		BPTR old = CurrentDir(wa[1].wa_Lock);
		struct SysStart pre;
		memset(&pre, 0, sizeof pre);
		pre.skip = -1;
		pre.joykeys = -1; pre.pad = -1; pre.speed = -1; pre.video = -1; pre.model = -1; pre.diskwrite = -1;
		pre.fasttape = -1; pre.tapesound = -1; pre.rtg = -1; pre.scan = -1; pre.autotape = -1; pre.hdmisound = -1; pre.rtglock = -1; pre.tone = -1;
#ifndef NO_USBPADS
		pre.usbpads = -1;
#endif
		if (IconBase && read_tooltypes(wa[1].wa_Name, &pre) && pre.prefs[0]) {
			char dir[SYS_PATH];
			if (strchr(pre.prefs, ':') == NULL && NameFromLock(wa[1].wa_Lock, (STRPTR)dir, SYS_PATH)) {
				AddPart((STRPTR)dir, (STRPTR)pre.prefs, SYS_PATH);
				strncpy(pre.prefs, dir, SYS_PATH - 1);
			}
			strcpy(st->prefs, pre.prefs);
			if (pre.menu) st->menu = 1;
			if (pre.hires) st->hires = 1;
			if (pre.tall) st->tall = 1;
			if (pre.wide) st->wide = 1;
			if (pre.nosound) st->nosound = 1;
			if (pre.skip >= 0) st->skip = pre.skip;
			if (pre.refresh > 0) st->refresh = pre.refresh;
			if (pre.joykeys >= 0) st->joykeys = pre.joykeys;
			if (pre.remap[0]) strcpy(st->remap, pre.remap);
			if (pre.pad >= 0) st->pad = pre.pad;
			if (pre.speed >= 0) st->speed = pre.speed;
			if (pre.video >= 0) st->video = pre.video;
			if (pre.model >= 0) st->model = pre.model;
			if (pre.diskwrite >= 0) st->diskwrite = pre.diskwrite;
			if (pre.fasttape >= 0) st->fasttape = pre.fasttape;
			if (pre.tapesound >= 0) st->tapesound = pre.tapesound;
			if (pre.autotape >= 0) st->autotape = pre.autotape;
			if (pre.hdmisound >= 0) st->hdmisound = pre.hdmisound;
#ifndef NO_USBPADS
			if (pre.usbpads >= 0) st->usbpads = pre.usbpads;
#endif
			if (pre.rtglock >= 0) st->rtglock = pre.rtglock;
			if (pre.tone >= 0) st->tone = pre.tone;
			if (pre.rtg >= 0) st->rtg = pre.rtg;
			if (pre.scan >= 0) st->scan = pre.scan;
			if (pre.tape[0]) strcpy(st->tape, pre.tape);
		} else if (NameFromLock(wa[1].wa_Lock, (STRPTR)st->media, SYS_PATH)) {
			AddPart((STRPTR)st->media, wa[1].wa_Name, SYS_PATH);
#ifndef NO_LUPA
			if (pre.rtg >= 0) st->rtg = pre.rtg;   /* Lupa: RTG / AGA on the game's own icon */
#endif
		}
		CurrentDir(old);
	}
	return 1;
}

#ifndef NO_LUPA
int  Sys_LupaOneGame;
char Sys_LupaProfile[16];

/* ENV:AmiEmu/Display (the machine-wide display choice, framework section 2.3 step 3) */
int Sys_EnvDisplay(void)
{
	char v[16];
	int i;
	if (GetVar((STRPTR)"AmiEmu/Display", (STRPTR)v, sizeof v, GVF_GLOBAL_ONLY) <= 0) return -1;
	for (i = 0; v[i]; i++) { if (v[i] >= 'a' && v[i] <= 'z') v[i] -= 32; if (v[i] == ' ' || v[i] == '\t' || v[i] == '\n') { v[i] = 0; break; } }
	return !strcmp(v, "AGA") ? 0 : !strcmp(v, "RTG") ? 1 : !strcmp(v, "AUTO") ? 2 : -1;
}

/* PROFILE=AUTO on a 68060: Arnold-060 (built for it) runs instead, with the same words, from
 * this program's drawer; we wait and hand back its return code. Shell starts only. */
#if !defined(__mc68060__)
/* a 68060 and PROGDIR:Arnold-060 there: its full name in dir */
static int find060(char *dir)
{
	BPTR l;
	if (!(SysBase->AttnFlags & (1 << 7))) return 0;   /* AFB_68060 */
	if (!NameFromLock(GetProgramDir(), (STRPTR)dir, SYS_PATH) || !AddPart((STRPTR)dir, (STRPTR)"Arnold-060", SYS_PATH)) return 0;
	if (!(l = Lock((STRPTR)dir, SHARED_LOCK))) return 0;
	UnLock(l);
	return 1;
}

/* one more word for the command line, quoted; 0 if it doesn't fit or has a quote */
static int add_word(char *cmd, int *n, int size, const char *w)
{
	if (strchr(w, '"') || *n + (int)strlen(w) + 4 >= size) return 0;
	*n += sprintf(cmd + *n, " \"%s\"", w);
	return 1;
}
#endif

/* Workbench: the icons' settings (as Sys_WBStart read them) become the words of a Shell start of
 * Arnold-060; input and output NIL:. A bare preset icon (no game) opens the menu, as here. */
int Sys_Rerun060WB(const struct SysStart *st)
{
#if defined(__mc68060__)
	(void)st;
	return -1;
#else
	static char cmd[2400];
	char dir[SYS_PATH], w[SYS_PATH + 16];
	int n, ok = 1;
	BPTR in, out;
	LONG r;
	if (!find060(dir)) return -1;
	n = sprintf(cmd, "\"%s\"", dir);
#define W(...) do { sprintf(w, __VA_ARGS__); ok &= add_word(cmd, &n, sizeof cmd, w); } while (0)
	if (st->prefs[0]) W("PREFS=%s", st->prefs);
	if (st->media[0]) W("%s", st->media);
	if (st->menu || !st->media[0]) W("MENU");
	if (st->hires) W("HIRES");
	if (st->tall) W("TALL");
	if (st->wide) W("WIDE");
	if (st->nosound) W("NOSOUND");
	if (st->skip >= 0) W("SKIP=%d", st->skip);
	if (st->refresh > 0) W("REFRESH=%d", st->refresh);
	if (st->joykeys >= 0) W("JOYKEYS=%d", st->joykeys);
	if (st->remap[0]) W("REMAP=%s", st->remap);
	if (st->pad >= 0) W("PAD=%d", st->pad);
	if (st->speed >= 0) W("SPEED=%d", st->speed);
	if (st->video >= 0) W("VIDEO=%d", st->video);
	if (st->model >= 0 && st->model < SYS_MODELS) W("MODEL=%s", Sys_ModelNames[st->model]);
	if (st->diskwrite >= 0) W("DISKWRITE=%d", st->diskwrite);
	if (st->fasttape >= 0) W("FASTTAPE=%d", st->fasttape);
	if (st->tapesound >= 0) W("TAPESOUND=%d", st->tapesound);
	if (st->autotape >= 0) W("AUTOTAPE=%d", st->autotape);
	if (st->hdmisound >= 0) W("HDMISOUND=%d", st->hdmisound);
#ifndef NO_USBPADS
	if (st->usbpads >= 0) W("USBPADS=%d", st->usbpads);
#endif
	if (st->rtglock >= 0) W("RTGLOCK=%d", st->rtglock);
	if (st->tone >= 0) W("TONE=%d", st->tone);
	if (st->rtg >= 0) W("RTG=%d", st->rtg);
	if (st->scan >= 0 && st->scan < SYS_SCANS) W("SCANLINES=%s", Sys_ScanWords[st->scan]);
	if (st->tape[0]) W("TAPE=%s", st->tape);
#ifndef NO_RTGVBL060
	W("RTGVBL=%d", Sys_RtgVblank ? 1 : 0);   /* 2.1: RTGVBL=0 too (on by default, so the ToolType's 0 was lost; issue #1) */
#else
	if (Sys_RtgVblank) W("RTGVBL=1");
#endif
	if (Sys_LupaOneGame) W("ONEGAME");
#undef W
	if (!ok) return -1;
	in = Open((STRPTR)"NIL:", MODE_OLDFILE);
	out = Open((STRPTR)"NIL:", MODE_NEWFILE);
	if (!in || !out) { if (in) Close(in); if (out) Close(out); return -1; }
	Sys_Log("68060: running %s", cmd);
	Sys_LogClose();                     /* Arnold-060 writes the log */
	r = SystemTags((STRPTR)cmd, SYS_Input, (ULONG)in, SYS_Output, (ULONG)out, TAG_DONE);
	Close(in); Close(out);
	if (r < 0) { Sys_LogOpen(); Sys_Log("68060: Arnold-060 didn't start, running here"); return -1; }
	return (int)r;
#endif
}

int Sys_Rerun060(int argc, char **argv)
{
#if defined(__mc68060__)
	(void)argc; (void)argv;
	return -1;                          /* this is Arnold-060 */
#else
	static char cmd[1200];
	char dir[SYS_PATH];
	int i, n;
	LONG r;
	if (!find060(dir)) return -1;
	n = sprintf(cmd, "\"%s\"", dir);
	for (i = 1; i < argc; i++) {
		if (strchr(argv[i], '"') || n + strlen(argv[i]) + 4 >= sizeof cmd) return -1;
		n += sprintf(cmd + n, " \"%s\"", argv[i]);
	}
	r = SystemTagList((STRPTR)cmd, NULL);   /* our own Input() and Output(), waits */
	return r < 0 ? -1 : (int)r;
#endif
}
#endif

/* ---- setting words shared by prefs, arguments and ToolTypes ---- */
const char *Sys_ModelNames[SYS_MODELS] = { "464", "664", "6128", "PLUS", "GX4000" };
const char *Sys_VideoNames[3] = { "AUTO", "PAL", "NTSC" };
const char *Sys_SpeedNames[2] = { "50", "DISPLAY" };
const char *Sys_ScanWords[SYS_SCANS] = { "off", "25", "50", "75", "blendlight", "blend", "blendstrong" };
static const char *scan_upper[SYS_SCANS] = { "OFF", "25", "50", "75", "BLENDLIGHT", "BLEND", "BLENDSTRONG" };
static const char *scan_hard[4] = { "0", "HARD25", "HARD50", "HARD75" };

static int word_index(const char *v, const char **names, int n)
{
	int i, k;
	for (i = 0; i < n; i++) {
		for (k = 0; names[i][k]; k++) {
			char c = v[k];
			if (c >= 'a' && c <= 'z') c -= 32;
			if (c != names[i][k]) break;
		}
		if (!names[i][k] && (v[k] == 0 || v[k] == ' ' || v[k] == '\t')) return i;
	}
	return -1;
}
int Sys_ParseModel(const char *v)
{
	int i = word_index(v, Sys_ModelNames, SYS_MODELS);
	if (i < 0 && v[0] == '6' && v[1] == '1' && v[2] == '2' && v[3] == '8' && (v[4] == '+' || v[4] == 'P' || v[4] == 'p')) i = SYS_MODEL_PLUS;
	return i;
}
int Sys_ParseVideo(const char *v)
{
	int i = word_index(v, Sys_VideoNames, 3);
	if (i < 0 && v[0] >= '0' && v[0] <= '2' && !v[1]) i = v[0] - '0';
	return i;
}
int Sys_ParseSpeed(const char *v)
{
	int i = word_index(v, Sys_SpeedNames, 2);
	if (i < 0 && (v[0] == '0' || v[0] == '1') && !v[1]) i = v[0] - '0';
	return i;
}
int Sys_ParseScan(const char *v)
{
	int i = word_index(v, scan_upper, SYS_SCANS);
	if (i < 0) i = word_index(v, scan_hard, 4);
	return i;
}
const char *Sys_ToneWords[3] = { "bright", "soft", "warm" };
static const char *tone_upper[3] = { "BRIGHT", "SOFT", "WARM" };
int Sys_ParseTone(const char *v)
{
	int i = word_index(v, tone_upper, 3);
	if (i < 0 && v[0] >= '0' && v[0] <= '2' && !v[1]) i = v[0] - '0';
	return i;
}
