/* Arnold core audio (16-bit stereo, native byte order) to two signed 8-bit Paula
 * channels. Shared by the Amiga host and the headless PC build, so the PC's WAV
 * dump checks the same conversion.
 * soundq (21 Oct 2026): the core's values are mix byte << 7 plus a 7-bit fraction
 * (0..32383, audioevent.c, unless NO_SNDLUT_FIX). With the AY's unipolar output
 * (psgplay.c, unless NO_SNDUNIPOLAR) the DC is removed first, as the CPC's output
 * capacitor does: a one-pole high-pass at about 14 Hz (y += dx - y / 256). */
#ifndef AUDIO_CONV_H
#define AUDIO_CONV_H
#ifndef NO_SNDLUT_FIX
#ifndef NO_SNDUNIPOLAR
static long Audio_DcX[2], Audio_DcY[2];   /* last input, output (<< 8) per side */
static int Audio_DcOn;
static signed char Audio_DcBlock(int side, long x)
{
	long y, o;
	if (!Audio_DcOn) { Audio_DcOn = 1; Audio_DcX[0] = Audio_DcX[1] = x; Audio_DcY[0] = Audio_DcY[1] = 0; }
	/* y in 1/256 of the input units (2^15 x 2^8 = 2^23, fits a long) */
	y = Audio_DcY[side] + ((x - Audio_DcX[side]) << 8) - (Audio_DcY[side] >> 8);
	Audio_DcX[side] = x; Audio_DcY[side] = y;
	o = (y + (1L << 14)) >> 15;   /* 1/256 units >> 7 -> 8-bit */
	if (o > 127) o = 127; else if (o < -128) o = -128;
	return (signed char)o;
}
#endif
#endif
static void Audio_SplitTo8(const unsigned short *s16, signed char *l, signed char *r, int samples)
{
	int i;
	for (i = 0; i < samples; i++) {
#ifdef NO_SNDLUT_FIX
		/* high byte 0..124, centred to -124..124 */
		l[i] = (signed char)((int)(s16[2 * i] >> 7) - 124);
		r[i] = (signed char)((int)(s16[2 * i + 1] >> 7) - 124);
#elif !defined(NO_SNDUNIPOLAR)
		l[i] = Audio_DcBlock(0, (long)s16[2 * i]);
		r[i] = Audio_DcBlock(1, (long)s16[2 * i + 1]);
#else
		/* 0..32383 rounded to 0..253, centred on 128 (silence of the bipolar mix) */
		int a = ((int)s16[2 * i] + 64) >> 7, b = ((int)s16[2 * i + 1] + 64) >> 7;
		l[i] = (signed char)((a > 255 ? 255 : a) - 128);
		r[i] = (signed char)((b > 255 ? 255 : b) - 128);
#endif
	}
}
#endif
