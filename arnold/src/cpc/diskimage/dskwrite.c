/*
 *  Arnold for Amiga: disk write-back (22 Oct 2026).
 *
 *  Turns what the emulated machine did to a disk (SAVE, high scores, FORMAT)
 *  back into the .dsk file it came from, in the file's own format:
 *
 *  - The original image is the template. A track whose sector IDs (C H R N)
 *    are as they were is copied from the file byte for byte, and only the
 *    sectors whose data or status bytes changed are patched in. So a standard
 *    DSK stays standard, an extended one stays extended, and anything this
 *    emulator does not model (weak sectors' extra copies, odd stored lengths,
 *    gap and filler bytes, the creator string) is kept.
 *  - A track that was formatted to a different layout, or added, is built
 *    from the core's copy. A standard DSK stays standard if every such track
 *    has one sector size and fits the image's track size; otherwise the image
 *    becomes extended (the only way it can hold the new layout).
 *  - A weak sector (an extended image storing several copies) that the
 *    machine rewrote gets the new data in every copy, as a real write would
 *    leave it: readable the same way every time.
 *
 *  The file side writes a temp file in the same folder and renames it over
 *  the original, so a failed write never leaves a half-written image, and the
 *  first write of an image keeps the untouched original as <name>.bak (never
 *  replacing an existing .bak). Plain stdio: rename() is not allowed to
 *  replace an existing file on AmigaOS, so the original is renamed away
 *  first (to .bak, or to .old which is deleted afterwards).
 *
 *  This file is GPL like the rest of Arnold.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "diskimg.h"
#include "iextdsk.h"
#include "dskwrite.h"
#define DSKWRITE_CLASH "disk name too long for this volume: changes kept in memory"

extern BOOL DiskImage_GetUnitPointer(int DriveID, DISKIMAGE_UNIT **ppUnit);

#define WB_TRACKS 85            /* the core's track list: 85 tracks x 2 sides */

static int sec_size(int N) { return (1 << (N & 7)) << 7; }
static int rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }

/* One track block of the original image, as the core's loaders read it */
typedef struct {
	long off;               /* offset of the Track-Info block in the file, -1 none */
	long size;              /* block size in the file */
	int spt;                /* IDs the loader used */
	long dataoff[64];       /* offset of each sector's data */
	int stored[64];         /* bytes stored for it */
} WB_TRACK;

typedef struct {
	int ext, tracks, sides;
	long stdsize;           /* standard: the one track size */
	WB_TRACK t[WB_TRACKS][2];
} WB_IMAGE;

static int parse_image(const unsigned char *d, unsigned long len, WB_IMAGE *im)
{
	long off = 0x100;
	int i, total;
	memset(im, 0, sizeof *im);
	if (len < 0x100) return 0;
	if (!memcmp(d, "EXTENDED", 8)) im->ext = 1;
	else if (memcmp(d, "MV - CPC", 8)) return 0;
	im->tracks = d[0x30]; im->sides = d[0x31];
	if (im->sides < 1 || im->sides > 2 || im->tracks > WB_TRACKS) return 0;
	im->stdsize = rd16(d + 0x32);
	{ int a, b; for (a = 0; a < WB_TRACKS; a++) for (b = 0; b < 2; b++) im->t[a][b].off = -1; }
	total = im->tracks * im->sides;
	for (i = 0; i < total; i++) {
		WB_TRACK *t = &im->t[i / im->sides][i % im->sides];
		long size = im->ext ? (long)d[0x34 + i] << 8 : im->stdsize;
		const unsigned char *th;
		long p; int j, spt;
		if (size == 0) continue;                      /* unformatted (extended) */
		if (off + size > (long)len || size < 0x100) { off += size; continue; }   /* missing: the core holds no data either */
		th = d + off;
		t->off = off; t->size = size;
		spt = th[0x15];
		if (im->ext) { if (spt > 29) { if (spt > 61) spt = 61; p = off + 512; } else p = off + 256; }
		else { if (spt > 29) spt = 29; p = off + 256; }
		/* the ID table must lie inside this track's block (an extended track with
		 * 30-61 IDs in a 256-byte block read up to 248 bytes past it, past the end
		 * of the file for the last track). Such a track then differs from the
		 * core's (same_layout) and is written from the core's copy. */
		if (0x18 + 8L * spt > size) spt = (int)((size - 0x18) / 8);
		t->spt = spt;
		for (j = 0; j < spt; j++) {
			const unsigned char *id = th + 0x18 + 8 * j;
			int idsize = sec_size(id[3]), l;
			if (im->ext) { l = rd16(id + 6); if (l == 0) l = idsize; t->dataoff[j] = p; t->stored[j] = l; p += l; }
			else { int stride = sec_size(th[0x14]); t->dataoff[j] = p + (long)stride * j; t->stored[j] = stride < idsize ? stride : idsize; }
			/* keep inside the block and the file, as the loaders' clamp does */
			if (t->dataoff[j] >= off + size) t->stored[j] = 0;
			else if (t->dataoff[j] + t->stored[j] > off + size) t->stored[j] = (int)(off + size - t->dataoff[j]);
			if (t->dataoff[j] + t->stored[j] > (long)len) t->stored[j] = t->dataoff[j] < (long)len ? (int)(len - t->dataoff[j]) : 0;
		}
		off += size;
	}
	return 1;
}

static EXTDSK_INTERNAL_TRACK *core_track(EXTDSK_INTERNAL *x, int track, int side)
{
	if (!x || !x->pTrackList || track >= WB_TRACKS) return NULL;
	return x->pTrackList[(track << 1) | side];
}

static int core_spt(EXTDSK_INTERNAL *x, int track, int side)
{
	EXTDSK_INTERNAL_TRACK *c = core_track(x, track, side);
	return c ? c->NoOfSectors : 0;
}

/* the same IDs in the same order as in the file: copy the file's block */
static int same_layout(const unsigned char *d, const WB_TRACK *t, EXTDSK_INTERNAL_TRACK *c)
{
	int j;
	if (t->off < 0) return c == NULL || c->NoOfSectors == 0;
	if (!c) return t->spt == 0;
	if (c->NoOfSectors != t->spt) return 0;
	for (j = 0; j < t->spt; j++) {
		const unsigned char *id = d + t->off + 0x18 + 8 * j;
		if (c->SectorIDs[j].C != id[0] || c->SectorIDs[j].H != id[1] || c->SectorIDs[j].R != id[2] || c->SectorIDs[j].N != id[3]) return 0;
	}
	return 1;
}

/* a track built from the core's copy (after a FORMAT): the block's size */
static long new_block_size(EXTDSK_INTERNAL_TRACK *c, int ext)
{
	long s = (c->NoOfSectors > 29) ? 512 : 256;
	int j;
	for (j = 0; j < c->NoOfSectors; j++) s += sec_size(c->SectorIDs[j].N);
	return ext ? (s + 255) & ~255L : s;
}

/* can a standard image hold this formatted track? one N, at most 29 IDs, fits */
static int std_fits(EXTDSK_INTERNAL_TRACK *c, long stdsize)
{
	int j;
	if (!c || c->NoOfSectors == 0) return 1;
	if (c->NoOfSectors > 29) return 0;
	for (j = 1; j < c->NoOfSectors; j++) if (c->SectorIDs[j].N != c->SectorIDs[0].N) return 0;
	return 256 + (long)c->NoOfSectors * sec_size(c->SectorIDs[0].N) <= stdsize;
}

static void put_new_block(unsigned char *o, EXTDSK_INTERNAL_TRACK *c, int track, int side, int ext)
{
	int j, n = c ? c->NoOfSectors : 0;
	long p = n > 29 ? 512 : 256;
	memcpy(o, "Track-Info\r\n", 12);
	o[0x10] = (unsigned char)track; o[0x11] = (unsigned char)side;
	o[0x14] = n ? c->SectorIDs[0].N : 2;
	o[0x15] = (unsigned char)n; o[0x16] = 0x4e; o[0x17] = 0xe5;
	for (j = 0; j < n; j++) {
		unsigned char *id = o + 0x18 + 8 * j;
		int sz = sec_size(c->SectorIDs[j].N);
		id[0] = c->SectorIDs[j].C; id[1] = c->SectorIDs[j].H; id[2] = c->SectorIDs[j].R; id[3] = c->SectorIDs[j].N;
		id[4] = c->SectorIDs[j].ST1; id[5] = c->SectorIDs[j].ST2;
		if (ext) { id[6] = (unsigned char)(sz & 0xff); id[7] = (unsigned char)(sz >> 8); }
		if (c->pSectorData[j]) memcpy(o + p, c->pSectorData[j], sz); else memset(o + p, 0xe5, sz);
		p += sz;
	}
}

/* copy a block from the file and patch in what the machine changed */
static void put_old_block(unsigned char *o, const unsigned char *d, const WB_TRACK *t, EXTDSK_INTERNAL_TRACK *c,
	int to_ext, int from_ext, long outsize, int *patched, int *weak)
{
	int j;
	memcpy(o, d + t->off, t->size < outsize ? t->size : outsize);
	if (to_ext && !from_ext) {
		/* standard -> extended: every sector stores the track's size, in place */
		int stride = sec_size(d[t->off + 0x14]);
		for (j = 0; j < t->spt; j++) { o[0x18 + 8 * j + 6] = (unsigned char)(stride & 0xff); o[0x18 + 8 * j + 7] = (unsigned char)(stride >> 8); }
	}
	if (!c) return;
	for (j = 0; j < t->spt; j++) {
		unsigned char *id = o + 0x18 + 8 * j;
		const unsigned char *src = (const unsigned char *)c->pSectorData[j];
		int sz = sec_size(c->SectorIDs[j].N), n = t->stored[j], changed = 0;
		long at = t->dataoff[j] - t->off;
		if (id[4] != c->SectorIDs[j].ST1 || id[5] != c->SectorIDs[j].ST2) { id[4] = c->SectorIDs[j].ST1; id[5] = c->SectorIDs[j].ST2; changed = 1; }
		if (src && n > 0) {
			int first = n < sz ? n : sz;
			if (memcmp(o + at, src, first)) {
				changed = 1;
				memcpy(o + at, src, first);
				if (n > sz && n % sz == 0) {      /* weak sector: n / sz copies, all rewritten */
					int k;
					for (k = 1; k < n / sz; k++) memcpy(o + at + (long)k * sz, src, sz);
					(*weak)++;
				}
			}
		}
		if (changed) (*patched)++;
	}
}

int DskWrite_Build(int drive, const unsigned char *orig, unsigned long origlen,
	unsigned char **pout, unsigned long *poutlen, char *why)
{
	static WB_IMAGE im;     /* about 70 KB: not on a 68k stack */
	DISKIMAGE_UNIT *u;
	EXTDSK_INTERNAL *x;
	int tracks, sides, ext, a, b, rebuilt = 0, patched = 0, weak = 0;
	unsigned long size;
	unsigned char *o, *w;

	*pout = NULL; *poutlen = 0; why[0] = 0;
	if (!DiskImage_GetUnitPointer(drive, &u) || !(u->Flags & DISKIMAGE_DISK_INSERTED)) { strcpy(why, "no disk"); return -1; }
	if (!(u->Flags & DISKIMAGE_DISK_DIRTY)) return 0;
	x = (EXTDSK_INTERNAL *)u->pDiskImage;
	if (!orig || !parse_image(orig, origlen, &im)) { strcpy(why, "not a DSK or extended DSK file (not written)"); return -1; }

	/* the size of the result */
	tracks = im.tracks; sides = im.sides;
	for (a = 0; a < WB_TRACKS; a++)
		for (b = 0; b < 2; b++)
			if (core_spt(x, a, b) > 0) { if (a + 1 > tracks) tracks = a + 1; if (b == 1) sides = 2; }
	ext = im.ext;
	for (a = 0; a < tracks; a++)
		for (b = 0; b < sides; b++) {
			const WB_TRACK *t = b < im.sides ? &im.t[a][b] : NULL;
			EXTDSK_INTERNAL_TRACK *c = core_track(x, a, b);
			if (t && t->off >= 0 && same_layout(orig, t, c)) continue;
			if ((!t || t->off < 0) && core_spt(x, a, b) == 0) continue;   /* still unformatted */
			rebuilt++;
			if (!ext && !std_fits(c, im.stdsize)) ext = 2;   /* standard can't hold it: becomes extended */
		}
	size = 0x100;
	for (a = 0; a < tracks; a++)
		for (b = 0; b < sides; b++) {
			const WB_TRACK *t = b < im.sides ? &im.t[a][b] : NULL;
			EXTDSK_INTERNAL_TRACK *c = core_track(x, a, b);
			long s;
			if (!ext) s = im.stdsize;
			else if (t && t->off >= 0 && same_layout(orig, t, c)) s = (t->size + 255) & ~255L;
			else s = (c && c->NoOfSectors) ? new_block_size(c, 1) : 0;
			if (s > 0xff00) { strcpy(why, "a formatted track is too big for a DSK (not written)"); return -1; }
			size += s;
		}

	o = (unsigned char *)calloc(1, size);
	if (!o) { strcpy(why, "out of memory (not written)"); return -1; }
	memcpy(o, orig, 0x100);
	if (ext == 2) {   /* a standard image turned extended */
		memset(o, 0, 0x100);
		memcpy(o, "EXTENDED CPC DSK File\r\nDisk-Info\r\n", 34);
		memcpy(o + 0x22, "Arnold", 6);
	}
	o[0x30] = (unsigned char)tracks; o[0x31] = (unsigned char)sides;
	if (ext) { o[0x32] = o[0x33] = 0; memset(o + 0x34, 0, 0x100 - 0x34); }
	w = o + 0x100;
	for (a = 0; a < tracks; a++)
		for (b = 0; b < sides; b++) {
			const WB_TRACK *t = b < im.sides ? &im.t[a][b] : NULL;
			EXTDSK_INTERNAL_TRACK *c = core_track(x, a, b);
			long s;
			if (t && t->off >= 0 && same_layout(orig, t, c)) {
				s = ext ? (t->size + 255) & ~255L : im.stdsize;
				put_old_block(w, orig, t, c, ext != 0, im.ext, s, &patched, &weak);
			} else {
				if (!ext) s = im.stdsize;
				else s = (c && c->NoOfSectors) ? new_block_size(c, 1) : 0;
				if (s) put_new_block(w, c, a, b, ext != 0);
			}
			if (ext) o[0x34 + a * sides + b] = (unsigned char)(s >> 8);
			w += s;
		}

	if (!patched && !rebuilt && ext == im.ext && tracks == im.tracks && sides == im.sides) { free(o); return 0; }   /* written, but nothing different */
	sprintf(why, "%d sector%s changed%s%s%s", patched, patched == 1 ? "" : "s",
		rebuilt ? ", tracks formatted" : "", weak ? ", weak sectors rewritten" : "",
		ext == 2 ? ", now an extended DSK (new track layout)" : "");
	*pout = o; *poutlen = size;
	return 1;
}

void DskWrite_Clean(int drive)
{
	DISKIMAGE_UNIT *u;
	if (DiskImage_GetUnitPointer(drive, &u)) u->Flags &= ~DISKIMAGE_DISK_DIRTY;
}

static int file_exists(const char *n)
{
	FILE *f = fopen(n, "rb");
	if (!f) return 0;
	fclose(f);
	return 1;
}

/* The temp, backup and old names must not be the image, each other, or a name
 * the volume stored cut short. A name that exists is checked now (Host_SameFile
 * / Host_NameKept); one that does not exist yet is checked again after the temp
 * file is created. 1 = refuse. */
static int name_clash(const char *name, const char *tmp, const char *bak, const char *old)
{
	const char *x[3]; int i, j;
	x[0] = tmp; x[1] = bak; x[2] = old;
	for (i = 0; i < 3; i++) {
		if (Host_SameFile(name, x[i])) return 1;
		if (file_exists(x[i]) && !Host_NameKept(x[i])) return 1;
		for (j = 0; j < i; j++) if (Host_SameFile(x[j], x[i])) return 1;
	}
	return 0;
}

int DskWrite_SaveFile(const char *name, const unsigned char *buf, unsigned long len, int *made_bak, char *why)
{
	char tmp[DSKWRITE_PATH + 8], bak[DSKWRITE_PATH + 8], old[DSKWRITE_PATH + 8];
	const char *moved;
	FILE *f;
	int ok;

	*made_bak = 0; why[0] = 0;
	if (strlen(name) >= DSKWRITE_PATH) { strcpy(why, "path too long"); return 0; }
	if (!Host_FileWritable(name)) { strcpy(why, "the file or disk is write-protected"); return 0; }
	sprintf(tmp, "%s.tmp", name); sprintf(bak, "%s.bak", name); sprintf(old, "%s.old", name);

	/* 0. the four names must be four different files (N1 of the code review).
	 * A volume that cuts names to 30 characters (OFS/FFS of older Kickstarts) makes
	 * .tmp/.bak/.old one file for a 29-character name and .tmp the image itself
	 * for a 30-character one. Nothing is removed or renamed until this is clear. */
	if (name_clash(name, tmp, bak, old)) { strcpy(why, DSKWRITE_CLASH); return 0; }

	/* 1. the new image into <name>.tmp, checked */
	remove(tmp);
	f = fopen(tmp, "wb");
	if (!f) { strcpy(why, "can't create the temp file (write-protected or name too long?)"); return 0; }
	ok = fwrite(buf, 1, len, f) == len;
	if (fclose(f) != 0) ok = 0;
	/* the temp file we just made must be stored under the name we gave it (and not be the image) */
	if (ok && (Host_SameFile(name, tmp) || !Host_NameKept(tmp))) {
		if (!Host_SameFile(name, tmp)) remove(tmp);   /* ours (the name did not exist before) */
		strcpy(why, DSKWRITE_CLASH); return 0;
	}
	if (ok) {
		f = fopen(tmp, "rb");
		ok = f && fseek(f, 0, SEEK_END) == 0 && ftell(f) == (long)len;
		if (f) fclose(f);
	}
	if (!ok) { remove(tmp); strcpy(why, "writing the temp file failed (disk full?)"); return 0; }

	/* 2. the original out of the way: the first time to .bak (kept), else .old */
	if (!file_exists(bak)) {
		if (rename(name, bak) != 0) { remove(tmp); strcpy(why, "can't rename the original to .bak"); return 0; }
		moved = bak; *made_bak = 1;
	} else {
		remove(old);
		if (rename(name, old) != 0) { remove(tmp); strcpy(why, "can't rename the original to .old"); return 0; }
		moved = old;
	}
	/* 3. the temp file becomes the image; on failure the original comes back */
	if (rename(tmp, name) != 0) {
		rename(moved, name);
		*made_bak = 0;
		remove(tmp);
		strcpy(why, "can't rename the temp file (original kept)");
		return 0;
	}
	/* 4. the old copy goes */
	if (moved == old) remove(old);
	return 1;
}

int DskWrite_Flush(int drive, const char *name, unsigned char **pimg, unsigned long *plen, char *msg)
{
	unsigned char *out; unsigned long outlen;
	char why[160], err[100];
	int r, bak;

	msg[0] = 0;
	r = DskWrite_Build(drive, *pimg, *plen, &out, &outlen, why);
	if (r == 0) { DskWrite_Clean(drive); return 0; }
	if (r < 0) { sprintf(msg, "not written: %s", why); return -1; }
	if (!DskWrite_SaveFile(name, out, outlen, &bak, err)) {
		sprintf(msg, "not written: %s", err);
		free(out);
		return -1;
	}
	free(*pimg);
	*pimg = out; *plen = outlen;      /* the file as it is now: the template for the next write */
	DskWrite_Clean(drive);
	sprintf(msg, "written, %lu bytes: %s%s", outlen, why, bak ? " (original kept as .bak)" : "");
	return 1;
}
