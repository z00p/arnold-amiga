/* Arnold on AmigaOS: save the last frame as IFF ILBM and PNG. Plain C, no
 * OS calls: the frame is the host's 8-bit chunky buffer plus its palette.
 * PNG: 8-bit indexed, zlib stream of stored (uncompressed) deflate blocks. */
#include <stdio.h>
#include <string.h>
#include "screenshot.h"

static int write_be32(FILE *f, unsigned long v)
{
	unsigned char b[4] = { v >> 24, v >> 16, v >> 8, v };
	return fwrite(b, 1, 4, f) == 4;
}

/* ---- ILBM: 8 planes, uncompressed, PAL hires or lores CAMG ---- */
int Screenshot_SaveILBM(const char *name, const unsigned char *chunky, int w, int h, int pitch,
                        const unsigned char pal[256][3], int hires)
{
	FILE *f = fopen(name, "wb");
	int rowbytes = ((w + 15) / 16) * 2, y, p, x;
	unsigned long body = (unsigned long)rowbytes * 8 * h;
	unsigned char row[128], bmhd[20];
	if (!f || rowbytes > (int)sizeof row) { if (f) fclose(f); return 0; }
	memset(bmhd, 0, sizeof bmhd);
	bmhd[0] = w >> 8; bmhd[1] = w; bmhd[2] = h >> 8; bmhd[3] = h;
	bmhd[8] = 8; bmhd[13] = 1; bmhd[14] = 1;   /* depth 8, no mask, no compression; aspect 1:1 */
	bmhd[16] = w >> 8; bmhd[17] = w; bmhd[18] = h >> 8; bmhd[19] = h;
	fwrite("FORM", 1, 4, f); write_be32(f, 4 + 8 + 20 + 8 + 768 + 8 + 4 + 8 + body);
	fwrite("ILBMBMHD", 1, 8, f); write_be32(f, 20); fwrite(bmhd, 1, 20, f);
	fwrite("CMAP", 1, 4, f); write_be32(f, 768); fwrite(pal, 1, 768, f);
	fwrite("CAMG", 1, 4, f); write_be32(f, 4); write_be32(f, 0x21000UL | (hires ? 0x8000UL : 0));
	fwrite("BODY", 1, 4, f); write_be32(f, body);
	for (y = 0; y < h; y++) {
		const unsigned char *src = chunky + (long)y * pitch;
		for (p = 0; p < 8; p++) {
			memset(row, 0, rowbytes);
			for (x = 0; x < w; x++) if (src[x] & (1 << p)) row[x >> 3] |= 0x80 >> (x & 7);
			fwrite(row, 1, rowbytes, f);
		}
	}
	fclose(f);
	return 1;
}

/* ---- PNG ---- */
static unsigned long crc_table[256];
static void crc_init(void)
{
	unsigned long c; int n, k;
	if (crc_table[1]) return;
	for (n = 0; n < 256; n++) {
		c = n;
		for (k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320UL ^ (c >> 1) : c >> 1;
		crc_table[n] = c;
	}
}
static unsigned long crc_update(unsigned long c, const unsigned char *p, unsigned long n)
{
	while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
	return c;
}

static int chunk(FILE *f, const char *type, const unsigned char *data, unsigned long len)
{
	unsigned long c;
	write_be32(f, len);
	fwrite(type, 1, 4, f);
	if (len) fwrite(data, 1, len, f);
	c = crc_update(0xffffffffUL, (const unsigned char *)type, 4);
	if (len) c = crc_update(c, data, len);
	return write_be32(f, c ^ 0xffffffffUL);
}

int Screenshot_SavePNG(const char *name, const unsigned char *chunky, int w, int h, int pitch,
                       const unsigned char pal[256][3])
{
	FILE *f = fopen(name, "wb");
	unsigned char ihdr[13], hdr[5], *line;
	unsigned long a = 1, b = 0, idatlen, crc, raw = (unsigned long)(w + 1) * h, done = 0;
	int y, x;
	static unsigned char buf[1 + 768];
	if (!f) return 0;
	crc_init();
	fwrite("\211PNG\r\n\032\n", 1, 8, f);
	ihdr[0] = w >> 24; ihdr[1] = w >> 16; ihdr[2] = w >> 8; ihdr[3] = w;
	ihdr[4] = h >> 24; ihdr[5] = h >> 16; ihdr[6] = h >> 8; ihdr[7] = h;
	ihdr[8] = 8; ihdr[9] = 3; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;   /* 8-bit indexed */
	chunk(f, "IHDR", ihdr, 13);
	chunk(f, "PLTE", &pal[0][0], 768);
	/* IDAT: zlib header, stored blocks (one per row, w+1 <= 65535), adler32. */
	idatlen = 2 + (unsigned long)h * (5 + w + 1) + 4;
	write_be32(f, idatlen); fwrite("IDAT", 1, 4, f);
	crc = crc_update(0xffffffffUL, (const unsigned char *)"IDAT", 4);
	buf[0] = 0x78; buf[1] = 0x01; fwrite(buf, 1, 2, f); crc = crc_update(crc, buf, 2);
	line = buf + 1;
	for (y = 0; y < h; y++) {
		const unsigned char *src = chunky + (long)y * pitch;
		unsigned len = w + 1;
		hdr[0] = (y == h - 1) ? 1 : 0;           /* BFINAL on the last block */
		hdr[1] = len; hdr[2] = len >> 8; hdr[3] = ~len; hdr[4] = (~len) >> 8;
		fwrite(hdr, 1, 5, f); crc = crc_update(crc, hdr, 5);
		buf[0] = 0;                               /* filter: none */
		for (x = 0; x < w; x++) line[x] = src[x];
		fwrite(buf, 1, len, f); crc = crc_update(crc, buf, len);
		for (x = 0; x < (int)len; x++) { a = (a + buf[x]) % 65521; b = (b + a) % 65521; }
		done += len;
	}
	{
		unsigned char ad[4] = { b >> 8, b, a >> 8, a };
		fwrite(ad, 1, 4, f); crc = crc_update(crc, ad, 4);
	}
	write_be32(f, crc ^ 0xffffffffUL);
	chunk(f, "IEND", NULL, 0);
	fclose(f);
	return done == raw;
}
