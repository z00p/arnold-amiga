#include "cassette.h"

#ifndef CASSETTE_NOSAMPLE
#include "sampload.h"
#endif

#include "tzx.h"


void	Cassette_Init(void)
{
//	TZX_Write_Initialise("out.tmp");
	TapeImage_Init();

}

void	Cassette_Finish(void)
{
//	TZX_Write_End();


}

static unsigned long CPC_CassetteType = CASSETTE_TYPE_NONE;

void	CPC_SetCassetteType(int CassetteType)
{
	CPC_CassetteType = CassetteType;
}

unsigned long CPC_GetCassetteType(void)
{
	return CPC_CassetteType;
}

unsigned long Cassette_Read(unsigned long NopsPassed)
{
	unsigned long CassetteReadBit = 0;

	switch (CPC_CassetteType)
	{
#ifndef CASSETTE_NOSAMPLE
		case CASSETTE_TYPE_SAMPLE:
		{
			CassetteReadBit = Sample_GetDataByteTimed(NopsPassed);
		}
		break;
#endif

		case CASSETTE_TYPE_TAPE_IMAGE:
		{
			unsigned long TStatesPassed;

			/* 4 t-states per NOP */
			TStatesPassed = NopsPassed<<2;
        

			CassetteReadBit = TapeImage_GetBit(TStatesPassed); if (TapeAudio_On) TapeAudio_Note((int)CassetteReadBit);
		}
		break;
	
		default:
			break;
	}

	return CassetteReadBit;
}


void	Cassette_Write(unsigned long NopsPassed, unsigned long State)
{
//	TZX_Write(NopsPassed, State);
}


			
void	Tape_Remove()
{
	TapeImage_Remove();
#ifndef CASSETTE_NOSAMPLE
	Sample_Close();
#endif
}


/* ---- Tape sound (Amiga port, 25 Oct 2026) ----------------------------------
 * The tape player is lazy: it only moves when the Z80 reads port B (see
 * cpc.c PPI_DoPortBInput), so the level is only known at those reads. Each
 * change of the level seen by a read is noted with the NOP count; a loader
 * polls every few NOPs, so a pulse edge is placed to within a few microseconds.
 * Once a frame the host mixes it into the finished sample block.
 * Like CPCEC (cpcec.c audio_main: the tape level, 0 or 4096, added to the AY
 * output of at most 3 x 10922) the level is a unipolar step of 1/8 of the
 * loudest mix, centre (both sides). Each output sample is the time-weighted
 * average of the level over its own 45 us (as the sub-sample AY render in
 * psgplay.c does for the tones), then a one-pole low-pass at about 5 kHz
 * takes the edge off what is left of the square's upper harmonics. The DC is
 * removed afterwards by audio_conv.h like the AY's.
 * At the end of the file so nothing above moves (PGO profile). */
#include "cpc.h"
#define TAPEAUDIO_STEP 4048   /* 32383 / 8 = CPCEC's 4096 of 32766 */
#define TAPEAUDIO_MAXEV 1024
int TapeAudio_On;
unsigned long TapeAudio_Changes;
static unsigned long ta_t[TAPEAUDIO_MAXEV];   /* NOP count of each change */
static unsigned char ta_b[TAPEAUDIO_MAXEV];   /* the level from then on */
static int ta_n;                              /* changes noted this frame */
static int ta_last = -1;                      /* last level noted (-1: none yet) */
static int ta_level = -1;                     /* the level at the start of the frame being mixed */
static int ta_lp;                             /* low-pass state */
static int ta_was;                            /* mixed in the previous frame */
static unsigned long ta_prev;                 /* NOP count at the end of the previous frame */

void	TapeAudio_Note(int bit)
{
	bit = bit != 0;
	if (bit == ta_last) return;
	ta_last = bit; TapeAudio_Changes++;
	if (ta_n < TAPEAUDIO_MAXEV) { ta_t[ta_n] = CPC_GetNopCount(); ta_b[ta_n] = (unsigned char)bit; ta_n++; }
}

void	TapeAudio_Reset(void)
{
	ta_was = 0; ta_n = 0; ta_level = -1; ta_lp = 0; ta_last = -1;
}

void	TapeAudio_Mix(unsigned short *s16, int samples)
{
	unsigned long now = CPC_GetNopCount(), span, step16, recip = 0;
	long s0 = 0, lastw = 0;
	int i, ev = 0, level, target, on = TapeAudio_On;

	if (!on || samples <= 0 || (long)(now - ta_prev) <= 0 || (long)(now - ta_prev) > 60000L || !ta_was) {
		/* off (fast tape, no tape, ...), the first frame, or the clock jumped: start again from here */
		ta_was = on; ta_prev = now; ta_n = 0; ta_level = -1; ta_lp = 0; ta_last = -1;
		return;
	}
	span = now - ta_prev;
	step16 = (span << 16) / (unsigned long)samples;   /* NOPs a sample, 16.16 (no divides in the loop) */
	level = ta_level;
	if (level < 0) level = ta_n ? ta_b[0] : 0;   /* unknown level: assume the first change is no step */
	target = CPC_GetTapeMotor();
	for (i = 0; i < samples; i++) {
		long e = i + 1 == samples ? (long)span : (long)(((unsigned long)(i + 1) * step16) >> 16), t = s0, hi = 0, w = e - s0, x;
		while (ev < ta_n) {
			long te = (long)(ta_t[ev] - ta_prev);
			if (te >= e) break;
			if (te < t) te = t;
			if (level) hi += te - t;
			t = te; level = ta_b[ev]; ev++;
		}
		if (level) hi += e - t;
		if (w != lastw && w > 0) { lastw = w; recip = ((unsigned long)TAPEAUDIO_STEP << 16) / (unsigned long)w; }   /* w is one of two values */
		x = (!target || w <= 0) ? 0 : (long)(((unsigned long)hi * recip) >> 16);
		ta_lp += (int)(((x - ta_lp) * 3) >> 2);   /* 3/4 per sample at 22.24 kHz: -3 dB at about 4.9 kHz */
		{
			unsigned long a = s16[2 * i] + (unsigned long)ta_lp, b = s16[2 * i + 1] + (unsigned long)ta_lp;
			s16[2 * i] = (unsigned short)(a > 65535UL ? 65535UL : a);
			s16[2 * i + 1] = (unsigned short)(b > 65535UL ? 65535UL : b);
		}
		s0 = e;
	}
	ta_level = level; ta_prev = now; ta_n = 0;
}

#ifndef NO_SAVESTATE
/* Amiga port (save states, 28 Oct 2026): the cassette type and the tape sound's
 * state for cpc/savestate.c (TapeAudio_On is the host's switch). */
#include "savestate.h"
int SS_TapeSampleInUse(void) { return CPC_CassetteType == CASSETTE_TYPE_SAMPLE; }
void SS_TapeAudio(SS_IO *io)
{
	long n = ta_n;
	SS_V(io, CPC_CassetteType); SS_V(io, TapeAudio_Changes); SS_V(io, ta_last); SS_V(io, ta_level);
	SS_V(io, ta_lp); SS_V(io, ta_was); SS_V(io, ta_prev);
	n = SS_Long(io, n);
	if (n < 0 || n > TAPEAUDIO_MAXEV) { SS_Fail(io, "The state is damaged (tape sound)"); return; }
	if (io->load) ta_n = (int)n;
	{
		int i;
		for (i = 0; i < n; i++) { SS_V(io, ta_t[i]); SS_V(io, ta_b[i]); }
	}
}
#endif
