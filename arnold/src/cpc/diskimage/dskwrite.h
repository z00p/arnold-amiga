/* Arnold for Amiga: disk write-back (22 Oct 2026), see dskwrite.c */
#ifndef __DSKWRITE_H__
#define __DSKWRITE_H__

#define DSKWRITE_PATH 256
#define DSKWRITE_MSG  320   /* size of the msg buffer DskWrite_Flush fills */

/* Build the new image for a drive from the file it was loaded from (orig).
 * 1: *pout (malloc'd, *poutlen bytes) holds it, why says what changed;
 * 0: nothing changed (the drive is not dirty, or wrote the same data);
 * -1: can't be written, why says why. */
int  DskWrite_Build(int drive, const unsigned char *orig, unsigned long origlen,
	unsigned char **pout, unsigned long *poutlen, char *why);
/* the drive's changes are in its file now */
void DskWrite_Clean(int drive);
/* Write buf over the file name safely: name.tmp, then the original renamed to
 * name.bak (only if there is none: *made_bak = 1) or name.old (deleted at the
 * end), then name.tmp renamed to name. 1 done, 0 failed (why), file untouched. */
int  DskWrite_SaveFile(const char *name, const unsigned char *buf, unsigned long len, int *made_bak, char *why);
/* Both: if the drive changed, write it to name. *pimg, *plen is the file's
 * content, replaced by the new image on success. 1 written, 0 nothing to do,
 * -1 failed (msg says why; the changes stay in memory). */
int  DskWrite_Flush(int drive, const char *name, unsigned char **pimg, unsigned long *plen, char *msg);

/* the host: 0 if the file or its volume is write-protected */
int  Host_FileWritable(const char *name);
/* 1 if both names exist and are the very same file (SameLock on the Amiga) */
int  Host_SameFile(const char *a, const char *b);
/* 0 if the file exists but the volume stored it under a shorter name than
 * asked for (names cut to 30 characters); 1 if fine or if it does not exist */
int  Host_NameKept(const char *name);

#endif
