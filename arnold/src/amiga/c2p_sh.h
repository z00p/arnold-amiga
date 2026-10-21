/* Shadow c2p shaped for Emu68 (perf50, 20 Oct 2026), included by amiga_sys.c
 * (and by tools/perf50/c2p_test.c, which checks it against the old loop).
 * Same chip writes and shadow as c2p_line_sh with known != 0: for each group
 * of 32 pixels all 8 plane longs are formed (planes the line does not use come
 * out 0, as there, since its pixels have those bits clear), compared with the
 * shadow, and only the differing ones written. Emu68 guesses a forward branch
 * as not taken and leaves the translated code when wrong: the old loop took
 * the "equal, no write" branch for most planes and tested np inside the pixel
 * loop. Here the pixel part has no branches and the usual "equal" case falls
 * through; the rare write is out of line. NO_EMUBR: the old loop. */
#define C2P_SH_BYTE(lo, hi, s) \
	lo  = c2p_lo[s[0]] << 7; hi  = c2p_hi[s[0]] << 7; \
	lo |= c2p_lo[s[1]] << 6; hi |= c2p_hi[s[1]] << 6; \
	lo |= c2p_lo[s[2]] << 5; hi |= c2p_hi[s[2]] << 5; \
	lo |= c2p_lo[s[3]] << 4; hi |= c2p_hi[s[3]] << 4; \
	lo |= c2p_lo[s[4]] << 3; hi |= c2p_hi[s[4]] << 3; \
	lo |= c2p_lo[s[5]] << 2; hi |= c2p_hi[s[5]] << 2; \
	lo |= c2p_lo[s[6]] << 1; hi |= c2p_hi[s[6]] << 1; \
	lo |= c2p_lo[s[7]];      hi |= c2p_hi[s[7]];
#define C2P_SH_MERGE(lo, hi) \
	o0 = (o0 << 8) | ( lo        & 0xff); o1 = (o1 << 8) | ((lo >>  8) & 0xff); \
	o2 = (o2 << 8) | ((lo >> 16) & 0xff); o3 = (o3 << 8) | ((lo >> 24) & 0xff); \
	o4 = (o4 << 8) | ( hi        & 0xff); o5 = (o5 << 8) | ((hi >>  8) & 0xff); \
	o6 = (o6 << 8) | ((hi >> 16) & 0xff); o7 = (o7 << 8) | ((hi >> 24) & 0xff);
#define C2P_SH_PUT(p, o) \
	if (__builtin_expect(s[(p) * ng] != (o), 0)) { s[(p) * ng] = (o); *(ULONG *)(plane[p] + g4) = (o); w++; }

static unsigned long c2p_sh_known(const unsigned char *src, UBYTE *const *plane, int ng, ULONG *sh)
{
	unsigned long w = 0;
	ULONG *s = sh;
	int g4 = 0;
	const unsigned char *end = src + (ng << 5);
	while (src != end)
	{
		ULONG o0 = 0, o1 = 0, o2 = 0, o3 = 0, o4 = 0, o5 = 0, o6 = 0, o7 = 0, lo, hi;
		C2P_SH_BYTE(lo, hi, src) C2P_SH_MERGE(lo, hi)
		C2P_SH_BYTE(lo, hi, (src + 8)) C2P_SH_MERGE(lo, hi)
		C2P_SH_BYTE(lo, hi, (src + 16)) C2P_SH_MERGE(lo, hi)
		C2P_SH_BYTE(lo, hi, (src + 24)) C2P_SH_MERGE(lo, hi)
		src += 32;
		C2P_SH_PUT(0, o0) C2P_SH_PUT(1, o1) C2P_SH_PUT(2, o2) C2P_SH_PUT(3, o3)
		C2P_SH_PUT(4, o4) C2P_SH_PUT(5, o5) C2P_SH_PUT(6, o6) C2P_SH_PUT(7, o7)
		s++;
		g4 += 4;
	}
	return w;
}
