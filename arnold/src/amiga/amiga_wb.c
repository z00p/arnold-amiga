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
	if (tt) st->pad = !strcmp(tt, "CD32") || atoi(tt) == 1;
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
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"TAPE");
	if (tt) strncpy(st->tape, tt, SYS_PATH - 1);   /* TAPE=<file.cdt> */
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
	st->joykeys = -1; st->pad = -1; st->speed = -1; st->video = -1; st->model = -1; st->diskwrite = -1; st->fasttape = -1; st->tapesound = -1;
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
		pre.fasttape = -1; pre.tapesound = -1;
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
			if (pre.tape[0]) strcpy(st->tape, pre.tape);
		} else if (NameFromLock(wa[1].wa_Lock, (STRPTR)st->media, SYS_PATH))
			AddPart((STRPTR)st->media, wa[1].wa_Name, SYS_PATH);
		CurrentDir(old);
	}
	return 1;
}

/* ---- setting words shared by prefs, arguments and ToolTypes ---- */
const char *Sys_ModelNames[SYS_MODELS] = { "464", "664", "6128", "PLUS", "GX4000" };
const char *Sys_VideoNames[3] = { "AUTO", "PAL", "NTSC" };
const char *Sys_SpeedNames[2] = { "50", "DISPLAY" };

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
