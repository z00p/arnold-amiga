/* Arnold core audio (16-bit stereo, VolumeLookup16Bit values 0..31744, native
 * byte order) to two signed 8-bit Paula channels. Shared by the Amiga host and
 * the headless PC build, so the PC's WAV dump checks the same conversion. */
#ifndef AUDIO_CONV_H
#define AUDIO_CONV_H
static void Audio_SplitTo8(const unsigned short *s16, signed char *l, signed char *r, int samples)
{
	int i;
	for (i = 0; i < samples; i++) {
		/* high byte 0..124, centred to -124..124 */
		l[i] = (signed char)((int)(s16[2 * i] >> 7) - 124);
		r[i] = (signed char)((int)(s16[2 * i + 1] >> 7) - 124);
	}
}
#endif
