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
	if (FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"NOSOUND")) st->nosound = 1;
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"SKIP");
	if (tt) st->skip = atoi(tt);
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"REFRESH");
	if (tt) st->refresh = atoi(tt);
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"JOYKEYS");
	if (tt) st->joykeys = atoi(tt);
	tt = (char *)FindToolType((STRPTR *)dobj->do_ToolTypes, (STRPTR)"PAD");
	if (tt) st->pad = !strcmp(tt, "CD32") || atoi(tt) == 1;
	FreeDiskObject(dobj);
	return 1;
}

int Sys_WBStart(int argc, char **argv, struct SysStart *st)
{
	struct WBStartup *wbs;
	struct WBArg *wa;
	memset(st, 0, sizeof *st);
	st->skip = -1;
	st->joykeys = -1; st->pad = -1;
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
		pre.joykeys = -1; pre.pad = -1;
		if (IconBase && read_tooltypes(wa[1].wa_Name, &pre) && pre.prefs[0]) {
			char dir[SYS_PATH];
			if (strchr(pre.prefs, ':') == NULL && NameFromLock(wa[1].wa_Lock, (STRPTR)dir, SYS_PATH)) {
				AddPart((STRPTR)dir, (STRPTR)pre.prefs, SYS_PATH);
				strncpy(pre.prefs, dir, SYS_PATH - 1);
			}
			strcpy(st->prefs, pre.prefs);
			if (pre.menu) st->menu = 1;
			if (pre.hires) st->hires = 1;
			if (pre.nosound) st->nosound = 1;
			if (pre.skip >= 0) st->skip = pre.skip;
			if (pre.refresh > 0) st->refresh = pre.refresh;
			if (pre.joykeys >= 0) st->joykeys = pre.joykeys;
			if (pre.pad >= 0) st->pad = pre.pad;
		} else if (NameFromLock(wa[1].wa_Lock, (STRPTR)st->media, SYS_PATH))
			AddPart((STRPTR)st->media, wa[1].wa_Name, SYS_PATH);
		CurrentDir(old);
	}
	return 1;
}
