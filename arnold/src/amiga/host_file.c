/* Arnold host for AmigaOS: the core's file access (src/cpc/host.h Host_*File,
 * Host_LoadFile / Host_SaveFile) on stdio. Split off host_amiga.c (split16, 30 Oct 2026). */
#include "../cpc/host.h"
#include <stdio.h>
#include <stdlib.h>

/* ---- files ---- */
HOST_FILE_HANDLE Host_OpenFile(const char *n, int Access) { return (HOST_FILE_HANDLE)fopen(n, Access == HOST_FILE_ACCESS_READ ? "rb" : "wb"); }
void Host_CloseFile(HOST_FILE_HANDLE h) { if (h) fclose((FILE *)h); }
int Host_GetFileSize(HOST_FILE_HANDLE h) { FILE *f = (FILE *)h; long p = ftell(f), s; fseek(f, 0, SEEK_END); s = ftell(f); fseek(f, p, SEEK_SET); return (int)s; }
void Host_WriteData(HOST_FILE_HANDLE h, unsigned char *d, unsigned long l) { fwrite(d, 1, l, (FILE *)h); }
void Host_ReadData(HOST_FILE_HANDLE h, unsigned char *d, unsigned long l) { fread(d, 1, l, (FILE *)h); }

BOOL Host_LoadFile(const char *n, unsigned char **pp, unsigned long *pl)
{
	FILE *f = fopen(n, "rb"); long s; unsigned char *p;
	*pp = NULL; *pl = 0;
	if (!f) return FALSE;
	fseek(f, 0, SEEK_END); s = ftell(f); fseek(f, 0, SEEK_SET);
	p = malloc(s);
	if (!p || fread(p, 1, s, f) != (size_t)s) { fclose(f); free(p); return FALSE; }
	fclose(f); *pp = p; *pl = s; return TRUE;
}
BOOL Host_SaveFile(const char *n, unsigned char *p, unsigned long l)
{
	FILE *f = fopen(n, "wb"); if (!f) return FALSE;
	fwrite(p, 1, l, f); fclose(f); return TRUE;
}
BOOL LoadFile(const char *n, unsigned char **pp, unsigned long *pl) { return Host_LoadFile(n, pp, pl); }
