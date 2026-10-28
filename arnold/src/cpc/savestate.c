/* Arnold for Amiga: exact save states (1.4, 28 Oct 2026), see savestate.h and
 * Docs/agent_notes/savestate.md.
 *
 * File: an SNA v3 snapshot (upstream's writer: 256-byte header, 64 or 128 KB
 * of RAM, the CPC+ chunk on a Plus) followed by the chunk "ARNS":
 *     "ARNS" <length, 4 bytes little-endian, as every SNA/RIFF chunk>
 *     "ArnoldSS" <format> <flags> <payload length> <CRC-32 of the payload>
 *     payload: sections "TAG " <length> <data>, all numbers big-endian
 * Integers are written as 1, 2 or 8 bytes (C types of 4 and 8 bytes both as 8,
 * since long is 4 bytes on the Amiga and 8 on the 64-bit PC). Pointers are not
 * stored: they become offsets or indices, or are worked out again on load.
 *
 * Load: the whole file is checked first (structure, CRC, format, machine,
 * media) and refused without any change if anything is wrong. Then the state
 * of the running machine is kept in memory (with its media) and the new one
 * applied; if that fails part way (it should not: the CRC is good), the kept
 * one is put back, so a load never leaves a half-loaded machine. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "savestate.h"
#include "cpcglob.h"
#include "cpc.h"
#include "snapshot.h"
#include "diskimage/diskimg.h"
#include "tzx.h"
#include "cassette.h"

#ifndef NO_SAVESTATE

unsigned long SaveState_Crc32(unsigned long crc, const unsigned char *p, unsigned long n)
{
	static unsigned long tab[256];
	static int init;
	if (!init) {
		unsigned long i, c; int k;
		for (i = 0; i < 256; i++) {
			c = i;
			for (k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
			tab[i] = c;
		}
		init = 1;
	}
	crc = ~crc & 0xFFFFFFFFUL;
	while (n--) crc = tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return ~crc & 0xFFFFFFFFUL;
}

/* ---- the stream ---- */
void SS_Fail(SS_IO *io, const char *msg)
{
	if (io->err) return;
	io->err = 1;
	strncpy(io->msg, msg, sizeof io->msg - 1);
	io->msg[sizeof io->msg - 1] = 0;
}

static void ss_put(SS_IO *io, const void *p, unsigned long n)
{
	if (io->err) return;
	if (io->pos + n > io->cap) {
		unsigned long cap = io->cap ? io->cap : 65536;
		unsigned char *b;
		while (cap < io->pos + n) cap *= 2;
		b = (unsigned char *)realloc(io->buf, cap);
		if (!b) { SS_Fail(io, "Not enough memory for the state"); return; }
		io->buf = b; io->cap = cap;
	}
	memcpy(io->buf + io->pos, p, n);
	io->pos += n;
	if (io->pos > io->len) io->len = io->pos;
}

static unsigned long ss_limit(SS_IO *io) { return io->depth ? io->end[io->depth - 1] : io->len; }

static int ss_get(SS_IO *io, void *p, unsigned long n)
{
	if (io->err) return 0;
	if (n > ss_limit(io) - io->pos) { SS_Fail(io, "The state is damaged (data ends early)"); return 0; }
	if (p) memcpy(p, io->buf + io->pos, n);
	io->pos += n;
	return 1;
}

void SS_Int(SS_IO *io, void *p, int size, int sign)
{
	unsigned char b[8];
	unsigned long long v = 0;
	int n = size >= 4 ? 8 : size, i;
	if (io->err) return;
	if (size != 1 && size != 2 && size != 4 && size != 8) { SS_Fail(io, "state: field of an unexpected size"); return; }
	if (!io->load) {
		if (size == 1) { unsigned char u = *(unsigned char *)p; v = sign ? (unsigned long long)(long long)(signed char)u : u; }
		else if (size == 2) { unsigned short u; memcpy(&u, p, 2); v = sign ? (unsigned long long)(long long)(short)u : u; }
		else if (size == 4) { unsigned int u; memcpy(&u, p, 4); v = sign ? (unsigned long long)(long long)(int)u : u; }
		else memcpy(&v, p, 8);
		for (i = n - 1; i >= 0; i--) { b[i] = (unsigned char)v; v >>= 8; }
		ss_put(io, b, n);
	} else {
		if (!ss_get(io, b, n)) return;
		for (i = 0; i < n; i++) v = (v << 8) | b[i];
		if (size == 1) *(unsigned char *)p = (unsigned char)v;
		else if (size == 2) { unsigned short u = (unsigned short)v; memcpy(p, &u, 2); }
		else if (size == 4) { unsigned int u = (unsigned int)v; memcpy(p, &u, 4); }
		else memcpy(p, &v, 8);
	}
}

long SS_Long(SS_IO *io, long v)
{
	long long x = v;
	SS_Int(io, &x, 8, 1);
	return io->load ? (long)x : v;
}

static unsigned long ss_ulong(SS_IO *io, unsigned long v)
{
	unsigned long long x = v & 0xFFFFFFFFUL;
	SS_Int(io, &x, 8, 0);
	return io->load ? (unsigned long)(x & 0xFFFFFFFFUL) : v;
}

void SS_Raw(SS_IO *io, void *p, unsigned long n)
{
	if (io->load) ss_get(io, p, n); else ss_put(io, p, n);
}

/* PackBits: control c < 128: c + 1 bytes follow; c > 128: the next byte 257 - c times */
void SS_Pack(SS_IO *io, void *p, unsigned long n)
{
	unsigned char *d = (unsigned char *)p;
	unsigned long len;
	if (io->err) return;
	if (!io->load) {
		unsigned char *o = (unsigned char *)malloc(n + n / 128 + 16);
		unsigned long i = 0, k = 0;
		if (!o) { SS_Fail(io, "Not enough memory for the state"); return; }
		while (i < n) {
			unsigned long j = i + 1;
			while (j < n && j - i < 128 && d[j] == d[i]) j++;
			if (j - i >= 3) { o[k++] = (unsigned char)(257 - (j - i)); o[k++] = d[i]; i = j; continue; }
			j = i;
			while (i < n && i - j < 128) {
				if (i + 2 < n && d[i] == d[i + 1] && d[i] == d[i + 2]) break;
				i++;
			}
			o[k++] = (unsigned char)(i - j - 1);
			memcpy(o + k, d + j, i - j); k += i - j;
		}
		ss_ulong(io, n);
		ss_ulong(io, k);
		ss_put(io, o, k);
		free(o);
	} else {
		unsigned long i = 0, end;
		if (ss_ulong(io, 0) != n) { SS_Fail(io, "The state is damaged (packed block of the wrong size)"); return; }
		len = ss_ulong(io, 0);
		if (io->err) return;
		if (len > ss_limit(io) - io->pos) { SS_Fail(io, "The state is damaged (packed block ends early)"); return; }
		end = io->pos + len;
		while (io->pos < end && !io->err) {
			unsigned int c = io->buf[io->pos++];
			if (c < 128) {
				if (c + 1 > n - i || c + 1 > end - io->pos) break;
				memcpy(d + i, io->buf + io->pos, c + 1); i += c + 1; io->pos += c + 1;
			} else if (c > 128) {
				if (257 - c > n - i || io->pos >= end) break;
				memset(d + i, io->buf[io->pos++], 257 - c); i += 257 - c;
			}
		}
		if (io->pos != end || i != n) SS_Fail(io, "The state is damaged (bad packed block)");
	}
}

static void put32be(unsigned char *p, unsigned long v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }
static unsigned long get32be(const unsigned char *p) { return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3]; }
static unsigned long get32le(const unsigned char *p) { return ((unsigned long)p[3] << 24) | ((unsigned long)p[2] << 16) | ((unsigned long)p[1] << 8) | p[0]; }
static void put32le(unsigned char *p, unsigned long v) { p[3] = (unsigned char)(v >> 24); p[2] = (unsigned char)(v >> 16); p[1] = (unsigned char)(v >> 8); p[0] = (unsigned char)v; }

void SS_Begin(SS_IO *io, const char *tag)
{
	unsigned char h[8];
	if (io->err) return;
	if (io->depth >= 8) { SS_Fail(io, "state: sections nested too deep"); return; }
	if (!io->load) {
		memcpy(h, tag, 4); put32be(h + 4, 0);
		io->start[io->depth++] = io->pos + 4;
		ss_put(io, h, 8);
	} else {
		unsigned long l;
		if (!ss_get(io, h, 8)) return;
		if (memcmp(h, tag, 4)) {
			char m[80];
			sprintf(m, "The state is damaged (section '%.4s' where '%.4s' belongs)", (char *)h, tag);
			SS_Fail(io, m);
			return;
		}
		l = get32be(h + 4);
		if (l > ss_limit(io) - io->pos) { SS_Fail(io, "The state is damaged (a section ends early)"); return; }
		io->end[io->depth++] = io->pos + l;
	}
}

void SS_End(SS_IO *io)
{
	if (io->err) return;
	if (io->depth <= 0) { SS_Fail(io, "state: section end without a start"); return; }
	io->depth--;
	if (!io->load) put32be(io->buf + io->start[io->depth], io->pos - io->start[io->depth] - 4);
	else if (io->pos != io->end[io->depth]) {
		char m[80];
		sprintf(m, "The state is damaged (a section is %ld bytes off)", (long)(io->end[io->depth] - io->pos));
		SS_Fail(io, m);
	}
}

static void ss_str(SS_IO *io, char *s, int size)
{
	long n = io->load ? 0 : (long)strlen(s);
	n = SS_Long(io, n);
	if (io->err) return;
	if (!io->load) { ss_put(io, s, n); return; }
	if (n < 0 || (unsigned long)n > ss_limit(io) - io->pos) { SS_Fail(io, "The state is damaged (bad name)"); return; }
	memcpy(s, io->buf + io->pos, n < size ? n : size - 1);
	s[n < size ? n : size - 1] = 0;
	io->pos += n;
}

/* the file part of a host path (Amiga "Work:Games/Sonic.cpr" or Unix "/x/y.cpr") */
static void ss_name(char *out, int size, const char *path)
{
	const char *s = path ? path : "", *p;
	for (p = s; *p; p++) if (*p == '/' || *p == ':' || *p == '\\') s = p + 1;
	strncpy(out, s, size - 1); out[size - 1] = 0;
}

/* ---- machine and media ---- */
#define SS_NAME 64
typedef struct
{
	SS_MACHINE m;
	char cart[SS_NAME];
	long disk_present[2], disk_embedded[2], disk_flags[2], disk_type[2];
	unsigned long disk_crc[2];
	char disk[2][SS_NAME];
	long tape_present, tape_embedded;
	unsigned long tape_crc, tape_size;
	char tape[SS_NAME];
} SS_MEDIA;

static void ss_machine(SS_IO *io, SS_MACHINE *m, char *cart)
{
	int i;
	SS_V(io, m->hardware); SS_V(io, m->disc); SS_V(io, m->ramexp); SS_V(io, m->ramconfig);
	SS_V(io, m->crtc); SS_V(io, m->extraram); SS_V(io, m->cart_crc);
	for (i = 0; i < 3; i++) SS_V(io, m->rom_crc[i]);
	ss_str(io, cart, SS_NAME);
}

static void ss_media(SS_IO *io, SS_MEDIA *md)
{
	int d;
	for (d = 0; d < 2; d++) {
		SS_V(io, md->disk_present[d]); SS_V(io, md->disk_embedded[d]); SS_V(io, md->disk_flags[d]);
		SS_V(io, md->disk_type[d]); SS_V(io, md->disk_crc[d]); ss_str(io, md->disk[d], SS_NAME);
	}
	SS_V(io, md->tape_present); SS_V(io, md->tape_embedded); SS_V(io, md->tape_crc);
	SS_V(io, md->tape_size); ss_str(io, md->tape, SS_NAME);
}

/* CRC-32 of a drive's disk as the state stores it (SS_DiskImage) */
static int ss_disk_crc(int drive, unsigned long *crc)
{
	SS_IO t;
	memset(&t, 0, sizeof t);
	SS_DiskImage(&t, drive);
	*crc = t.err ? 0 : SaveState_Crc32(0, t.buf, t.pos);
	free(t.buf);
	return !t.err;
}

static void ss_gather(SS_MEDIA *md, const SAVESTATE_INFO *info, int embed_all)
{
	int d, type;
	memset(md, 0, sizeof *md);
	SS_GetMachine(&md->m);
	if (info) ss_name(md->cart, SS_NAME, info->cart_name);
	for (d = 0; d < 2; d++) {
		int f = SS_DiskImageFlags(d, &type, 0, 0, 0);
		/* stored without "changed": a loaded copy is marked changed (below), and a state
		 * saved again straight after loading is the same file */
		md->disk_flags[d] = embed_all ? f : (f & ~DISKIMAGE_DISK_DIRTY); md->disk_type[d] = type;
		md->disk_present[d] = (f & DISKIMAGE_DISK_INSERTED) != 0;
		if (md->disk_present[d]) {
			ss_disk_crc(d, &md->disk_crc[d]);
			/* John, 28 Oct 2026: every inserted disk travels with the state, so a load is
			 * never refused for a disk (the game may have written to it since) */
			md->disk_embedded[d] = 1;
		}
		if (info) ss_name(md->disk[d], SS_NAME, info->disk_name[d]);
	}
	md->tape_present = SS_TapeCrc(&md->tape_crc, &md->tape_size);
	md->tape_embedded = md->tape_present && embed_all;
	if (info) ss_name(md->tape, SS_NAME, info->tape_name);
}

static const char *ss_model(const SS_MACHINE *m)
{
	if (m->hardware == CPC_HW_CPCPLUS) return m->disc ? "CPC 6128 Plus" : "GX4000";
	if (!m->disc) return "CPC 464";
	return m->ramexp ? "CPC 6128" : "CPC 664";
}

/* the checks before a load: 1 = this machine can take it */
static int ss_compatible(const SS_MEDIA *s, char *msg, int msglen)
{
	SS_MEDIA c;
	char m[300];
	int d;
	ss_gather(&c, NULL, 0);
	m[0] = 0;
	if (s->m.hardware != c.m.hardware || s->m.disc != c.m.disc || s->m.ramexp != c.m.ramexp)
		sprintf(m, "This state is for a %s; the machine running is a %s.", ss_model(&s->m), ss_model(&c.m));
	else if (s->m.ramconfig != c.m.ramconfig || s->m.crtc != c.m.crtc || s->m.extraram != c.m.extraram)
		sprintf(m, "This state is for a %s set up differently (RAM or CRTC).", ss_model(&s->m));
	else if (s->m.cart_crc != c.m.cart_crc)
		sprintf(m, "This state is for another cartridge: '%.60s' (CRC %08lX).", s->cart[0] ? s->cart : "?", s->m.cart_crc);
	else if (s->m.rom_crc[0] != c.m.rom_crc[0] || s->m.rom_crc[1] != c.m.rom_crc[1] || s->m.rom_crc[2] != c.m.rom_crc[2])
		sprintf(m, "This state was saved with other system ROMs.");
	for (d = 0; d < 2 && !m[0]; d++)
		if (s->disk_present[d] && !s->disk_embedded[d] && (!c.disk_present[d] || s->disk_crc[d] != c.disk_crc[d]))
			sprintf(m, "This state needs disk '%.60s' in drive %c, as it was when saved%s.", s->disk[d][0] ? s->disk[d] : "?", 'A' + d,
				c.disk_present[d] ? " (the disk in it now is another one or has changed since)" : "");
	if (!m[0] && s->tape_present && !s->tape_embedded && (!c.tape_present || s->tape_crc != c.tape_crc))
		sprintf(m, "This state needs tape '%.60s' in the cassette deck.", s->tape[0] ? s->tape : "?");
	if (!m[0]) return 1;
	if (msg && msglen > 0) { strncpy(msg, m, msglen - 1); msg[msglen - 1] = 0; }
	return 0;
}

/* ---- the whole state ---- */
static int ss_restoring;   /* 1 while the kept machine is put back after a failed load */
static void ss_stream(SS_IO *io, SS_MEDIA *md, const SAVESTATE_INFO *info, const unsigned char **phost, unsigned long *phostlen)
{
	int d;
	SS_Begin(io, "MACH"); ss_machine(io, &md->m, md->cart); SS_End(io);
	SS_Begin(io, "MEDI"); ss_media(io, md); SS_End(io);
	if (io->err) return;
	for (d = 0; d < 2; d++) {
		if (md->disk_embedded[d]) {
			SS_Begin(io, d ? "DSK1" : "DSK0");
			SS_DiskImage(io, d);
			/* loaded: marked changed, so the host's write-back puts it in its .dsk file
			 * (not when the running machine is being put back after a failed load) */
			SS_DiskImageFlags(d, NULL, io->load, (int)md->disk_flags[d] | (io->load && !ss_restoring ? DISKIMAGE_DISK_DIRTY : 0), (int)md->disk_type[d]);
			SS_End(io);
		} else if (io->load && !md->disk_present[d] && (SS_DiskImageFlags(d, NULL, 0, 0, 0) & DISKIMAGE_DISK_INSERTED))
			DiskImage_RemoveDisk(d);   /* the drive was empty */
		else if (io->load && md->disk_present[d])
			SS_DiskImageFlags(d, NULL, 1, (int)md->disk_flags[d], (int)md->disk_type[d]);
	}
	if (md->tape_embedded) {
		SS_Begin(io, "TIMG");
		if (!io->load) {
			unsigned long crc, size;
			extern unsigned char *SS_TapeData(void);
			SS_TapeCrc(&crc, &size);
			SS_Pack(io, SS_TapeData(), size);
		} else {
			unsigned char *t = (unsigned char *)malloc(md->tape_size ? md->tape_size : 1);
			if (!t) SS_Fail(io, "Not enough memory for the tape");
			SS_Pack(io, t, md->tape_size);
			if (!io->err) {
				TapeImage_Remove();
				if (TapeImage_Insert(t, md->tape_size) != ARNOLD_STATUS_OK) SS_Fail(io, "The state's tape could not be put back");
			}
			free(t);
		}
		SS_End(io);
	} else if (io->load && !md->tape_present) {
		unsigned long crc, size;
		if (SS_TapeCrc(&crc, &size)) TapeImage_Remove();   /* the deck was empty */
	}
	SS_Begin(io, "Z80 "); SS_Z80(io); SS_End(io);
	SS_Begin(io, "CPC "); SS_Cpc(io); SS_End(io);
	SS_Begin(io, "GA  "); SS_GateArray(io); SS_End(io);
	SS_Begin(io, "PAL "); SS_Pal(io); SS_End(io);
	SS_Begin(io, "ASIC"); SS_Asic(io); SS_End(io);
	SS_Begin(io, "CRTC"); SS_Crtc(io); SS_End(io);
	SS_Begin(io, "PSG "); SS_Psg(io); SS_End(io);
	SS_Begin(io, "PSGP"); SS_PsgPlay(io); SS_End(io);
	SS_Begin(io, "AUDI"); SS_Audio(io); SS_End(io);
	SS_Begin(io, "FDC "); SS_Fdc(io); SS_End(io);
	SS_Begin(io, "FDD "); SS_Fdd(io); SS_End(io);
	SS_Begin(io, "FDI "); SS_Fdi(io); SS_End(io);
	SS_Begin(io, "PRNT"); SS_Printer(io); SS_End(io);
	SS_Begin(io, "TAPE"); SS_Tape(io); SS_End(io);
	SS_Begin(io, "TAUD"); SS_TapeAudio(io); SS_End(io);
	SS_Begin(io, "REND"); SS_Render(io); SS_End(io);
	SS_Begin(io, "LOOP"); SS_Arnold(io); SS_End(io);
	SS_Begin(io, "HOST");
	{
		long n = SS_Long(io, info && info->host ? (long)info->hostlen : 0);
		if (!io->load) { if (n) ss_put(io, info->host, n); }
		else if (!io->err) {
			if (n < 0 || (unsigned long)n > ss_limit(io) - io->pos) SS_Fail(io, "The state is damaged (host data)");
			else { if (phost) { *phost = io->buf + io->pos; *phostlen = n; } io->pos += n; }
		}
	}
	SS_End(io);
	if (io->load && !io->err) {
		extern void GateArray_RethinkMemory(void);
		GateArray_RethinkMemory();   /* paging from the restored ROM/RAM/ASIC settings */
	}
	if (!io->err && io->load && io->pos != io->len) SS_Fail(io, "The state is damaged (data after the end)");
}

static void ss_copymsg(char *msg, int msglen, const char *m)
{
	if (msg && msglen > 0) { strncpy(msg, m, msglen - 1); msg[msglen - 1] = 0; }
}

/* the current machine as a payload (save, and the copy kept while loading) */
static int ss_build(SS_IO *io, const SAVESTATE_INFO *info, int embed_all)
{
	SS_MEDIA md;
	SS_MACHINE m;
	memset(io, 0, sizeof *io);
	SS_GetMachine(&m);
	if (m.hardware != CPC_HW_CPC && m.hardware != CPC_HW_CPCPLUS) { SS_Fail(io, "Save states need a CPC or a CPC Plus (not a KC Compact)"); return 0; }
	if (SS_TapeSampleInUse()) { SS_Fail(io, "Save states can't hold a sampled (WAV/VOC) tape; use a .cdt"); return 0; }
	ss_gather(&md, info, embed_all);
	ss_stream(io, &md, info, NULL, NULL);
	return !io->err;
}

int SaveState_Save(unsigned char **pData, unsigned long *pLen, const SAVESTATE_INFO *info, char *msg, int msglen)
{
	SS_IO io;
	SS_MACHINE m;
	unsigned long snalen, total;
	unsigned char *b, *p;
	int memk;
	*pData = NULL; *pLen = 0;
	if (!ss_build(&io, info, 0)) { ss_copymsg(msg, msglen, io.msg); free(io.buf); return 0; }
	SS_GetMachine(&m);
	memk = m.ramexp ? 128 : 64;
	snalen = Snapshot_CalculateOutputSize(memk, 3);
	total = snalen + 8 + 24 + io.pos;
	b = (unsigned char *)malloc(total);
	if (!b) { ss_copymsg(msg, msglen, "Not enough memory for the state"); free(io.buf); return 0; }
	memset(b, 0, snalen);
	Snapshot_GenerateOutputData(b, memk, 3);
	p = b + snalen;
	memcpy(p, "ARNS", 4); put32le(p + 4, 24 + io.pos);
	memcpy(p + 8, "ArnoldSS", 8);
	put32be(p + 16, SAVESTATE_FORMAT); put32be(p + 20, 0);
	put32be(p + 24, io.pos); put32be(p + 28, SaveState_Crc32(0, io.buf, io.pos));
	memcpy(p + 32, io.buf, io.pos);
	free(io.buf);
	*pData = b; *pLen = total;
	ss_copymsg(msg, msglen, "");
	return 1;
}

/* finds the ARNS payload: 1 = found and sound, 2 = a plain SNA (no ARNS), 0 = refused */
static int ss_find(const unsigned char *d, unsigned long len, const unsigned char **pay, unsigned long *paylen, char *m)
{
	unsigned long memsize, pos;
	int version;
	if (!d || len < 256 || memcmp(d, "MV - SNA", 8)) { strcpy(m, "Not a CPC snapshot (.sna)"); return 0; }
	version = d[0x10];
	if (version < 1 || version > 3) { sprintf(m, "Snapshot version %d is not supported", version); return 0; }
	memsize = ((unsigned long)d[0x6b] | ((unsigned long)d[0x6c] << 8)) << 10;
	if (version < 3) return 2;
	if (len < 256 + memsize) { strcpy(m, "The snapshot is damaged (shorter than its RAM size)"); return 0; }
	pos = 256 + memsize;
	while (pos < len) {
		unsigned long cl;
		if (len - pos < 8) { strcpy(m, "The snapshot is damaged (a chunk is cut off)"); return 0; }
		cl = get32le(d + pos + 4);
		if (cl > len - pos - 8) { strcpy(m, "The snapshot is damaged (a chunk is cut off)"); return 0; }
		if (!memcmp(d + pos, "ARNS", 4)) {
			const unsigned char *h = d + pos + 8;
			unsigned long f, l;
			if (cl < 24 || memcmp(h, "ArnoldSS", 8)) { strcpy(m, "The save state is damaged (bad ARNS chunk)"); return 0; }
			f = get32be(h + 8); l = get32be(h + 16);
			if (f != SAVESTATE_FORMAT) { sprintf(m, "This save state was made by another version of Arnold (state format %lu, this one reads %d)", f, SAVESTATE_FORMAT); return 0; }
			if (l != cl - 24) { strcpy(m, "The save state is damaged (wrong length)"); return 0; }
			if (SaveState_Crc32(0, h + 24, l) != get32be(h + 20)) { strcpy(m, "The save state is damaged (CRC error)"); return 0; }
			*pay = h + 24; *paylen = l;
			return 1;
		}
		pos += 8 + cl;
	}
	return 2;
}

/* the start of the payload: machine and media, without touching anything */
static int ss_peek(const unsigned char *pay, unsigned long paylen, SS_MEDIA *md, char *m)
{
	SS_IO io;
	memset(&io, 0, sizeof io);
	memset(md, 0, sizeof *md);
	io.load = 1; io.buf = (unsigned char *)pay; io.len = paylen;
	SS_Begin(&io, "MACH"); ss_machine(&io, &md->m, md->cart); SS_End(&io);
	SS_Begin(&io, "MEDI"); ss_media(&io, md); SS_End(&io);
	if (io.err) { strcpy(m, io.msg); return 0; }
	return 1;
}

/* a plain snapshot: upstream's loader copies everything after the first 64 KB
 * into the expansion RAM, so a file must not hold more than that */
static int ss_plain_ok(unsigned long len, char *m)
{
	SS_MACHINE mc;
	SS_GetMachine(&mc);
	if (len > 256 + 65536 && len - 256 - 65536 > (unsigned long)mc.extraram) { strcpy(m, "The snapshot is too big for this machine's RAM"); return 0; }
	return 1;
}

int SaveState_Check(const unsigned char *data, unsigned long len, char *msg, int msglen)
{
	const unsigned char *pay; unsigned long paylen;
	char m[300];
	SS_MEDIA md;
	int r = ss_find(data, len, &pay, &paylen, m);
	if (r == 2) { if (!ss_plain_ok(len, m)) { ss_copymsg(msg, msglen, m); return 0; } ss_copymsg(msg, msglen, ""); return 2; }
	if (r == 0 || !ss_peek(pay, paylen, &md, m) || !ss_compatible(&md, m, sizeof m)) { ss_copymsg(msg, msglen, m); return 0; }
	ss_copymsg(msg, msglen, "");
	return 1;
}

int SaveState_Load(const unsigned char *data, unsigned long len, char *msg, int msglen, SAVESTATE_LOADED *out)
{
	const unsigned char *pay; unsigned long paylen;
	char m[300];
	SS_MEDIA md, keep, before;
	SS_IO backup, io;
	const unsigned char *host = NULL; unsigned long hostlen = 0;
	int r = ss_find(data, len, &pay, &paylen, m), d;
	if (out) memset(out, 0, sizeof *out);
	if (r == 2) {
		if (!ss_plain_ok(len, m)) { ss_copymsg(msg, msglen, m); return SAVESTATE_REFUSED; }
		if (Snapshot_Insert(data, len) != ARNOLD_STATUS_OK) { ss_copymsg(msg, msglen, "The snapshot could not be loaded"); return SAVESTATE_REFUSED; }
		ss_copymsg(msg, msglen, "Plain snapshot: loaded the upstream way (not exact)");
		return SAVESTATE_PLAIN;
	}
	if (r == 0 || !ss_peek(pay, paylen, &md, m) || !ss_compatible(&md, m, sizeof m)) { ss_copymsg(msg, msglen, m); return SAVESTATE_REFUSED; }
	/* keep the running machine (with all its media) to put back if this fails */
	if (!ss_build(&backup, NULL, 1)) { ss_copymsg(msg, msglen, backup.msg); free(backup.buf); return SAVESTATE_REFUSED; }
	ss_gather(&before, NULL, 0);
	memset(&io, 0, sizeof io);
	io.load = 1; io.buf = (unsigned char *)pay; io.len = paylen;
	ss_stream(&io, &md, NULL, &host, &hostlen);
	if (io.err) {
		SS_IO back;
		memset(&back, 0, sizeof back);
		back.load = 1; back.buf = backup.buf; back.len = backup.pos;
		ss_restoring = 1;
		if (ss_peek(backup.buf, backup.pos, &keep, m)) ss_stream(&back, &keep, NULL, NULL, NULL);
		else back.err = 1;
		ss_restoring = 0;
		sprintf(m, "%.150s%s", io.msg, back.err ? " (and the machine could not be put back: Power ON)" : " (nothing changed)");
		free(backup.buf);
		ss_copymsg(msg, msglen, m);
		return SAVESTATE_REFUSED;
	}
	free(backup.buf);
	if (out) {
		out->host = host; out->hostlen = hostlen;
		for (d = 0; d < 2; d++) {
			out->disk[d] = md.disk_embedded[d] ? 2 : (!md.disk_present[d] && before.disk_present[d]) ? 1 : 0;
			strcpy(out->disk_name[d], md.disk[d]);
		}
		out->tape = !md.tape_present && before.tape_present;
		strcpy(out->cart_name, md.cart); strcpy(out->tape_name, md.tape);
	}
	ss_copymsg(msg, msglen, "");
	return SAVESTATE_EXACT;
}

#endif /* NO_SAVESTATE */
