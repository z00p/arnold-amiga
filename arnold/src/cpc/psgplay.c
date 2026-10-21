/* 
 *  Arnold emulator (c) Copyright, Kevin Thacker 1995-2001
 *  
 *  This file is part of the Arnold emulator source code distribution.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
/* code to emulate the sound */

#include "psgplay.h"
#include "audioevent.h"
#include "psg.h"
#include "headers.h"

static int	AY_VolumeTranslation[]= {0,0,0,1,1,1,2,3,4,6,8,10,14,19,24,31};

/* these are only updated when the audio event update requests it */
static int		PSGPlay_Registers[16];

static unsigned long             VolumeLookup8Bit[32];


static int PSGPlay_ChannelOutputVolumes[3];

/* soundq (21 Oct 2026). Sound fixes, each with its NO_x switch:
 * NO_SNDLEVELS   the AY's 16 levels as amplitudes 0..124 (the ayumi DAC curve x 124,
 *                rounded). Arnold's table above is the same curve x 31 rounded, so
 *                levels 0-2 and 3-5 were equal: volume-register sound (digi) lost
 *                detail at low levels.
 * NO_SNDENV_FIX  hardware envelope: patterns built per the datasheet (the decay table
 *                was overwritten with the attack one, the pattern pointer was
 *                shape x 48 in a table of 32-byte patterns, so shapes 11-15 read past
 *                it), one-shot and hold shapes stay at their end level instead of
 *                restarting every 32 steps, and the steps run at 1 MHz / (16 EP)
 *                (they ran at the tone toggle rate, an octave high).
 * NO_SNDUNIPOLAR a channel outputs 0 or its level, as the AY does (it was +- level
 *                around the middle: tones 6 dB louder than volume-register sound).
 *                The host removes the DC as the CPC's output capacitor does.
 * NO_SNDNOISE_FIX the noise generator shifts at 1 MHz / (16 NP), once a period (it ran
 *                twice as fast, and only on an odd count of periods in a sample).
 * NO_SNDAA       each output sample is the average of the PSG output over its 45 us
 *                (the time each tone is high, and register writes at their own time
 *                within the sample) instead of one point: square waves no longer
 *                alias, line-rate (15.6 kHz) volume writes no longer jitter. */
#ifndef NO_SNDLEVELS
static const int PSG_LevelAmp[16] = { 0, 1, 2, 3, 4, 6, 8, 13, 16, 25, 36, 46, 61, 79, 100, 124 };
#else
static const int PSG_LevelAmp[16] = { 0, 0, 0, 4, 4, 4, 8, 12, 16, 24, 32, 40, 56, 76, 96, 124 };
#endif
#ifndef NO_SNDUNIPOLAR
/* channel output, 0..252: 4 + 0 or 2 x level */
#define PSG_CHANNEL_OUT(On, Amp) ((On) ? 4 + 2 * (Amp) : 4)
#else
#define PSG_CHANNEL_OUT(On, Amp) ((On) ? 0x080 + (Amp) : 0x080 - (Amp))
#endif
#ifndef NO_SNDENV_FIX
/* envelope patterns hold levels 0..15 */
#define PSG_ENV_AMP(v) PSG_LevelAmp[v]
#else
#define PSG_ENV_AMP(v) VolumeLookup8Bit[v]
#endif

typedef struct
{
	/* tone period for this channel. Period is time for 1/2
	the square wave */
	unsigned long Period;

	/* Period position - used to define where in waveform we are. */
	FIXED_POINT16 PeriodPosition;
	/* state of square wave. 0x00 or 0x0ff */
	int		WaveFormState;
	FIXED_POINT16	ToneUpdate;
} CHANNEL_PERIOD;


CHANNEL_PERIOD	ChannelPeriods[3]=
{
	
	{0,{{0}},0x0ff,{{0}}},
	{0,{{0}},0x0ff,{{0}}},
	{0,{{0}},0x0ff,{{0}}}

};

static 	FIXED_POINT16	NoisePeriod;
static FIXED_POINT16 NoisePeriodPosition;
static int RNG=1;
static int NoiseOutput = 0x0ff;
static int NewNoisePeriod = 0;
static FIXED_POINT16 NewNoiseUpdate;
static FIXED_POINT16 NoiseUpdate;

static unsigned short EnvelopePeriod= 0;



void	InitChannelTone(int ChannelIndex);
void	InitNoisePeriod(void);


void	PSG_Envelope_Initialise(void);
void	PSG_Envelope_SetPeriod(void);


static FIXED_POINT16	Update;

void	PSG_InitialiseToneUpdates(FIXED_POINT16 *pUpdate)
{
	Update.FixedPoint.L = pUpdate->FixedPoint.L;
}

void	PSGPlay_Reset(void)
{
	int i;

	NoiseUpdate.FixedPoint.L = 1;

	ChannelPeriods[0].WaveFormState = 0xff;
	ChannelPeriods[1].WaveFormState = 0xff;
	ChannelPeriods[2].WaveFormState = 0xff;

	RNG=1;
	NoiseOutput=0x0ff;


   for (i=0; i<32; i++)
                {
					VolumeLookup8Bit[i] = (i<<2);	
               
				}
}

void	PSGPlay_ReloadChannelTone(int ChannelIndex)
{
	unsigned long CurrentCount;
	unsigned long Period;
	CHANNEL_PERIOD *pChannelPeriod = &ChannelPeriods[ChannelIndex];

	Period = (
		PSGPlay_Registers[(ChannelIndex<<1)] |
		(PSGPlay_Registers[(ChannelIndex<<1)+1]<<8)
		);

	if (Period == 0)
		Period = 1;

	/* calculate current count */
	CurrentCount = (pChannelPeriod->PeriodPosition.FixedPoint.L*pChannelPeriod->Period)>>16;

	if (Period<CurrentCount)
	{
		/* reset counter */
		pChannelPeriod->PeriodPosition.FixedPoint.L &= 0x0ffff;
	}

	/* set new period */
	pChannelPeriod->Period = Period; 
	
	if (Period<5)
	{
		/* if period <5 is programmed I can't hear any sound  */
		/* but it is possible to hear sound up to 0x0fff */
		pChannelPeriod->ToneUpdate.FixedPoint.L = 0;
	}
	else
	{
		/* set new update */
		pChannelPeriod->ToneUpdate.FixedPoint.L = Update.FixedPoint.L/Period;
	}
}


void	PSG_UpdateState(unsigned long Reg, unsigned long Data)
{
	/* write register data */
	PSGPlay_Registers[Reg & 0x0f] = Data;

	switch(Reg)
	{
		case 0:
		case 1:
		{
			PSGPlay_ReloadChannelTone(0);
		}
		break;

		case 2:
		case 3:
		{
			PSGPlay_ReloadChannelTone(1);

		}
		break;

		case 4:
		case 5:
		{
			PSGPlay_ReloadChannelTone(2);
		}
		break;

		case 6:
		{

			InitNoisePeriod();
		}
		break;

		case 11:
		case 12:
			PSG_Envelope_SetPeriod();
			break;

		case 13:
		{
			PSG_Envelope_Initialise();
		}
		break;

		case 7:
		{


		}
		break;

		case 8:
		case 9:
		case 10:
			{
				/* setup the output volume - to remove extra lookup */
				
				/* use hardware envelope? */
				if ((Data & (1<<4))!=0)
				{
					/* no */
					PSGPlay_ChannelOutputVolumes[Reg-8] = AY_VolumeTranslation[Data & 0x0f];
				}

			}
			break;
	}
}


static unsigned char Attack[16]=
{
	 0,  1,  2,  3,	 4,  5, 6, 7, 8, 9,10,11,12,13,14,15
};

static unsigned char Decay[16]=
{
	15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0
};

static unsigned char EnvelopePatterns[16*(16*2)];

/*
* 0	0	-	-	\________________________
*

* 0	1	-	-	/|_______________________
*

* 1	0	0	0	\|\|\|\|\|\|\|\|\|\|\|\|\

* 1	0	0	1	\________________________
*

* 1	0	1	0	\/\/\/\/\/\/\/\/\/\/\/\/		

*				  _______________________
* 1	0	1	1	\|

* 1	1	0	0	/|/|/|/|/|/|/|/|/|/|/|/|/|

*				 _________________________
* 1	1	0	1	/

* 1	1	1	0	/\/\/\/\/\/\/\/\/\/\/\/\/\

* 1	1	1	1	/|________________________
*
*/

void	PSG_InitialiseEnvelopeShapes(void)
{
	int i;

	/* TEMP TO REMOVE THIS LOOKUP */
	for (i=0; i<16; i++)
	{
		Attack[i] = AY_VolumeTranslation[i];
	}

	for (i=0; i<16; i++)
	{
		Decay[i] = AY_VolumeTranslation[i];
	}

	for (i=0; i<16; i++)
	{
		unsigned char *pEnvelopeData;
		unsigned char *pEnvelope;

		pEnvelopeData = &EnvelopePatterns[i*(16*2)];

		if (i & PSG_ENVELOPE_ATTACK)
		{
			pEnvelope = Attack;
		}
		else
		{
			pEnvelope = Decay;
		}

		memcpy(pEnvelopeData, pEnvelope, 16);
		pEnvelopeData+=16;

		if ((i & PSG_ENVELOPE_CONTINUE)==0)
		{
			memset(pEnvelopeData, 0, (16*1));
		}
		else
		{

			/* hold envelope at a value ? */
			if (i & PSG_ENVELOPE_HOLD)
			{
				unsigned char Volume;

				Volume = (pEnvelopeData-1)[0];

				if (i & PSG_ENVELOPE_ALTERNATE)
				{
					Volume = (15-Volume) & 15;
				}

				memset(pEnvelopeData, Volume, (16*1));
			}
			else
			{

				if (i & PSG_ENVELOPE_ALTERNATE)
				{
					/* change envelope pattern */
					if (pEnvelope==Attack)
					{
						pEnvelope = Decay;
					}
					else if (pEnvelope==Decay)
					{
						pEnvelope = Attack;
					}
				}

				memcpy(pEnvelopeData, pEnvelope,16);
				pEnvelopeData+=16;
#if 0
				if (i & PSG_ENVELOPE_ALTERNATE)
				{
					/* change envelope pattern */
					if (pEnvelope==Attack)
					{
						pEnvelope = Decay;
					}
					else if (pEnvelope==Decay)
					{
						pEnvelope = Attack;
					}
				}
				memcpy(pEnvelopeData, pEnvelope,16);
#endif		
			}
		}

	}
}




#ifndef NO_SNDENV_FIX
/* soundq: patterns rebuilt (see the top): 16 steps of the first ramp, then 16 of
   what follows (level 0, the hold level, the same ramp or the reverse one) */
static void PSG_RebuildEnvelopeShapes(void)
{
	int i, j;
	for (i=0; i<16; i++)
	{
		unsigned char *d = &EnvelopePatterns[i*32];
		int Up = (i & PSG_ENVELOPE_ATTACK) != 0;
		for (j=0; j<16; j++) d[j] = Up ? j : 15-j;
		for (j=0; j<16; j++)
		{
			if (!(i & PSG_ENVELOPE_CONTINUE)) d[16+j] = 0;
			else if (i & PSG_ENVELOPE_HOLD) d[16+j] = (Up ^ ((i & PSG_ENVELOPE_ALTERNATE) != 0)) ? 15 : 0;
			else if (i & PSG_ENVELOPE_ALTERNATE) d[16+j] = Up ? 15-j : j;
			else d[16+j] = d[j];
		}
	}
}
#endif

static int EnvelopePhase = 0;

static FIXED_POINT16 EnvelopePeriodPosition;

static int PositionInEnvelope = 0;
static char *pEnvelope = (char *)Attack;
static int EnvelopeVolume = 0;
static int CurrentEnvelope;
static FIXED_POINT16	EnvelopeUpdate;
#ifndef NO_SNDENV_FIX
static int EnvelopeRepeats;
#endif

void	PSG_Envelope_Initialise(void)
{
	unsigned long EnvelopeTimes16;
	
	CurrentEnvelope = PSGPlay_Registers[13];
	EnvelopeTimes16 = (CurrentEnvelope & 0x0f)<<4;
#ifndef NO_SNDENV_FIX
	pEnvelope = (char *)&EnvelopePatterns[EnvelopeTimes16<<1];   /* 32 bytes a shape */
	/* continue without hold: the 32 steps repeat; else stay in the second half */
	EnvelopeRepeats = (CurrentEnvelope & (PSG_ENVELOPE_CONTINUE|PSG_ENVELOPE_HOLD)) == PSG_ENVELOPE_CONTINUE;
	EnvelopePeriodPosition.FixedPoint.L = 0;
	EnvelopeVolume = pEnvelope[0];
#else
	pEnvelope = (char *)&EnvelopePatterns[EnvelopeTimes16 + (EnvelopeTimes16<<1)];
#endif
	EnvelopePhase = 0;
	PositionInEnvelope = 0;
}

void	PSG_Envelope_SetPeriod(void)
{
	unsigned long CurrentCount;
	unsigned long Period = (PSGPlay_Registers[11] & 0x0ff) |
					((PSGPlay_Registers[12] & 0x0ff)<<8);

	/* calculate current count */
	CurrentCount = (EnvelopePeriodPosition.FixedPoint.L*EnvelopePeriod)>>16;

	if (Period<CurrentCount)
	{
		/* reset counter */
		EnvelopePeriodPosition.FixedPoint.L &= 0x0ffff;
	}

	/* set new period */
	EnvelopePeriod = Period; 

	/* set new update */

	/* on amstrad I can't tell the difference between Period of 0 and period of 1 */
	if (Period==0)
	{
		Period = 1;
	}
#ifndef NO_SNDENV_FIX
	/* the envelope steps at 1 MHz / (16 EP), half the tone toggle rate (1 MHz / (8 TP)) */
	EnvelopeUpdate.FixedPoint.L = Update.FixedPoint.L/(Period<<1);
#else
	EnvelopeUpdate.FixedPoint.L = Update.FixedPoint.L/Period;
#endif
}


static INLINE void	Envelope_Update(FIXED_POINT16 *pUpdate)
{
	int NoOfCycles;

	/* update position in period */
	EnvelopePeriodPosition.FixedPoint.L +=EnvelopeUpdate.FixedPoint.L;	
	
	/* update number of cycles actually processed */
	NoOfCycles = (EnvelopePeriodPosition.FixedPoint.L>>16);	
	
	EnvelopePeriodPosition.FixedPoint.L &= 0x0ffff;
	
	/* exceeded 16 cycles = one whole wave complete. */
	if (NoOfCycles>=16)
	{
		EnvelopePhase = 0;
	}
		
	/* update position in envelope */
	PositionInEnvelope = (PositionInEnvelope + NoOfCycles);

#ifndef NO_SNDENV_FIX
	if (PositionInEnvelope > 31 && !EnvelopeRepeats) PositionInEnvelope = 31;
#endif
	PositionInEnvelope = PositionInEnvelope & 0x1f;

	EnvelopeVolume = pEnvelope[(EnvelopePhase<<4) + PositionInEnvelope];
}

static INLINE void	UpdateChannelToneState(int ChannelIndex, FIXED_POINT16 *pUpdate)
{
	CHANNEL_PERIOD *pChannelPeriod = &ChannelPeriods[ChannelIndex];

	int NoOfCycles;

	pChannelPeriod->PeriodPosition.FixedPoint.L+=pChannelPeriod->ToneUpdate.FixedPoint.L;

	/* update position in waveform */

	/* how many cycles have occured in this update ? */
	NoOfCycles = (pChannelPeriod->PeriodPosition.FixedPoint.L>>16);	

	/* if odd, invert state, else keep state the same */
	if (NoOfCycles & 0x01)
	{
		/* odd number of cycles - invert state*/
		pChannelPeriod->WaveFormState^=0x0ff;
	}

	/* zeroise integer part */
	pChannelPeriod->PeriodPosition.FixedPoint.L &= 0x0ffff;
}

static INLINE void	NoiseChooseOutput(void)
{
	/* Is noise output going to change? */
	if ((RNG + 1) & 2)	/* (bit0^bit1)? */
	{
		NoiseOutput = NoiseOutput^0x0ff;
	}

	/* The Random Number Generator of the 8910 is a 17-bit shift */
	/* register. The input to the shift register is bit0 XOR bit2 */
	/* (bit0 is the output). */

	/* The following is a fast way to compute bit 17 = bit0^bit2. */
	/* Instead of doing all the logic operations, we only check */
	/* bit 0, relying on the fact that after two shifts of the */
	/* register, what now is bit 2 will become bit 0, and will */
	/* invert, if necessary, bit 16, which previously was bit 18. */
	if (RNG & 1) RNG ^= 0x28000;
	RNG >>= 1;
}


void	UpdateNoise(FIXED_POINT16 *pUpdate)
{
	int NoOfCycles;

	NoisePeriodPosition.FixedPoint.L += NoiseUpdate.FixedPoint.L;	
	
	/* how many cycles have occured in this update ? */
	NoOfCycles = NoisePeriodPosition.FixedPoint.L>>16;	
	
	NoisePeriodPosition.FixedPoint.L &= 0x0ffff;
	
#ifndef NO_SNDNOISE_FIX
	if (NoOfCycles)
	{
		do NoiseChooseOutput(); while (--NoOfCycles > 0);
		NoisePeriod.FixedPoint.L = NewNoisePeriod<<16;
		NoiseUpdate.FixedPoint.L = NewNoiseUpdate.FixedPoint.L;
	}
#else
	/* if odd, invert state, else keep state the same */
	if (NoOfCycles & 0x01)
	{
		NoiseChooseOutput();
		NoisePeriod.FixedPoint.L = NewNoisePeriod<<16;
		NoiseUpdate.FixedPoint.L = NewNoiseUpdate.FixedPoint.L;
	}
#endif
}

void	InitNoisePeriod(void)
{
	int Period = (PSGPlay_Registers[6] & 0x01f);

	if (Period==0)
	{
		Period = 1;
	}

	NewNoisePeriod = Period;
#ifndef NO_SNDNOISE_FIX
	/* soundq: the noise shift register is clocked at 1 MHz / (16 NP), half the tone
	   toggle rate (it ran at the toggle rate, and only on an odd count of periods) */
	NewNoiseUpdate.FixedPoint.L = Update.FixedPoint.L/(Period<<1);
#else
	NewNoiseUpdate.FixedPoint.L = Update.FixedPoint.L/Period;
#endif
}

static INLINE int	GetMixedOutputForChannel(int ChannelIndex)
{
	unsigned long Mixer = PSGPlay_Registers[7];
	unsigned long ChannelMask = (1<<ChannelIndex);
	unsigned long ChannelToneOutput, ChannelNoiseOutput;

	{
		unsigned long Mixer_ToneOutput;
		
		/* tone enable */
		if ((Mixer & ChannelMask)==0)
		{
			/* tone enabled */
			Mixer_ToneOutput = 0;
		}
		else
		{
			Mixer_ToneOutput = -1;
		}

		ChannelToneOutput = ChannelPeriods[ChannelIndex].WaveFormState | Mixer_ToneOutput;
	}

	ChannelMask = ChannelMask<<3;

	{
		unsigned long Mixer_NoiseOutput;
	
		/* noise enable */
		if ((Mixer & ChannelMask)==0)
		{
			/* noise enabled */
			Mixer_NoiseOutput = 0;
		}
		else
		{
			Mixer_NoiseOutput = -1;
		}

		ChannelNoiseOutput = NoiseOutput | Mixer_NoiseOutput;
	}

	return ChannelToneOutput & ChannelNoiseOutput;
}

static INLINE int	GetFinalVolumeForChannel(int ChannelIndex)
{
	unsigned long ChannelOutput;
	int ChannelOutputVolume;
	int ChannelVolume;

	/* get programmed volume */
	ChannelVolume = PSGPlay_Registers[8+ChannelIndex];

	/* get state of waveform */
	ChannelOutput = GetMixedOutputForChannel(ChannelIndex);
	
	
	if ((ChannelVolume & 0x010)!=0)
	{
		/* envelope controls volume */
		ChannelOutputVolume = PSG_CHANNEL_OUT(ChannelOutput!=0x00, PSG_ENV_AMP(EnvelopeVolume));
	}
	else
	{
		/* volume used is the programmed volume */
		ChannelOutputVolume = PSG_CHANNEL_OUT(ChannelOutput!=0x00, PSG_LevelAmp[ChannelVolume & 0x0f]);
	}
	
	/* return volume for that channel */
	return ChannelOutputVolume;

}


PSG_OUTPUT_VOLUME		PSG_UpdateChannels(FIXED_POINT16 *pPeriodUpdate)
{
	PSG_OUTPUT_VOLUME PSG_Output={0,0,0,0};

	/* update envelope */
	Envelope_Update(pPeriodUpdate);
	
	UpdateNoise(pPeriodUpdate);

	/* update tones */
	UpdateChannelToneState(0, pPeriodUpdate);
	UpdateChannelToneState(1, pPeriodUpdate);
	UpdateChannelToneState(2, pPeriodUpdate);

	PSG_Output.VolA = GetFinalVolumeForChannel(0);
	PSG_Output.VolB = GetFinalVolumeForChannel(1);
	PSG_Output.VolC = GetFinalVolumeForChannel(2);

	return PSG_Output;
}


#ifndef NO_SNDAA
/* soundq: a per-sample step (16.16) scaled to a span of the sample (16.16, 1..0x10000);
   the steps are below 2^19 */
static INLINE unsigned long PSG_SpanStep(unsigned long Step, unsigned long Span)
{
	if (Span >= 0x10000) return Step;
	return (Step * (Span >> 4)) >> 12;
}

/* advance a tone over the span; returns the share of it the output was high (0..32768) */
static INLINE unsigned long PSG_ToneSpan(CHANNEL_PERIOD *pChannelPeriod, unsigned long Span)
{
	unsigned long u = PSG_SpanStep(pChannelPeriod->ToneUpdate.FixedPoint.L, Span);
	unsigned long x0 = pChannelPeriod->PeriodPosition.FixedPoint.L & 0x0ffff;
	unsigned long x1 = x0 + u;
	unsigned long k = x1 >> 16, High, m;
	int s = pChannelPeriod->WaveFormState != 0;

	pChannelPeriod->PeriodPosition.FixedPoint.L = x1 & 0x0ffff;
	if (k == 0) return s ? 32768 : 0;
	/* k half periods end in the span: the first piece (to the first toggle) has
	   state s, then k-1 whole ones alternating, then the last partial one */
	if (k & 1) pChannelPeriod->WaveFormState ^= 0x0ff;
	m = k - 1;
	High = s ? 0x10000 - x0 : 0;
	if (s ^ (int)(k & 1)) High += x1 & 0x0ffff;
	High += (s ? (m >> 1) : ((m + 1) >> 1)) << 16;
	return (High << 15) / u;   /* High <= u < 2^17 */
}

/* render the PSG over a span of an output sample: left and right (0..32256, 1/128 of
   the 0..252 mix units), the average over the span */
unsigned long PSG_RenderSpan(unsigned long Span, unsigned long *pRight)
{
	unsigned long Mixer = PSGPlay_Registers[7];
	long Out[3];
	unsigned long NoiseHigh;
	int c;

	/* envelope and noise: as PSG_UpdateChannels, the state at the span's end */
	{
		int NoOfCycles;
		EnvelopePeriodPosition.FixedPoint.L += PSG_SpanStep(EnvelopeUpdate.FixedPoint.L, Span);
		NoOfCycles = (EnvelopePeriodPosition.FixedPoint.L>>16);
		EnvelopePeriodPosition.FixedPoint.L &= 0x0ffff;
		if (NoOfCycles)
		{
			PositionInEnvelope += NoOfCycles;
#ifndef NO_SNDENV_FIX
			if (PositionInEnvelope > 31 && !EnvelopeRepeats) PositionInEnvelope = 31;
#endif
			PositionInEnvelope &= 0x1f;
		}
		EnvelopeVolume = pEnvelope[(EnvelopePhase<<4) + PositionInEnvelope];
	}
#ifndef NO_SNDNOISE_FIX
	/* noise: the share of the span its output is high (0..32768), as for a tone */
	{
		unsigned long u = PSG_SpanStep(NoiseUpdate.FixedPoint.L, Span);
		unsigned long x0 = NoisePeriodPosition.FixedPoint.L & 0x0ffff, x1 = x0 + u;
		unsigned long k = x1 >> 16;
		NoisePeriodPosition.FixedPoint.L = x1 & 0x0ffff;
		if (k == 0)
			NoiseHigh = NoiseOutput ? 32768 : 0;
		else
		{
			unsigned long High = NoiseOutput ? 0x10000 - x0 : 0;
			while (--k > 0) { NoiseChooseOutput(); if (NoiseOutput) High += 0x10000; }
			NoiseChooseOutput();
			if (NoiseOutput) High += x1 & 0x0ffff;
			NoisePeriod.FixedPoint.L = NewNoisePeriod<<16;
			NoiseUpdate.FixedPoint.L = NewNoiseUpdate.FixedPoint.L;
			/* High <= u; u reaches 2^18 at NP 1 (keep High << 15 in 32 bits) */
			NoiseHigh = u < 0x20000 ? (High << 15) / u : (High << 13) / (u >> 2);
		}
	}
#else
	{
		int NoOfCycles;
		NoisePeriodPosition.FixedPoint.L += PSG_SpanStep(NoiseUpdate.FixedPoint.L, Span);
		NoOfCycles = NoisePeriodPosition.FixedPoint.L>>16;
		NoisePeriodPosition.FixedPoint.L &= 0x0ffff;
		if (NoOfCycles & 0x01)
		{
			NoiseChooseOutput();
			NoisePeriod.FixedPoint.L = NewNoisePeriod<<16;
			NoiseUpdate.FixedPoint.L = NewNoiseUpdate.FixedPoint.L;
		}
		NoiseHigh = NoiseOutput ? 32768 : 0;
	}
#endif
	for (c = 0; c < 3; c++)
	{
		unsigned long On = PSG_ToneSpan(&ChannelPeriods[c], Span);   /* always: keeps the phase */
		int Vol = PSGPlay_Registers[8+c];
		long Amp = (Vol & 0x010) ? PSG_ENV_AMP(EnvelopeVolume) : PSG_LevelAmp[Vol & 0x0f];
		if (Mixer & (1<<c)) On = 32768;                           /* tone off: high */
		if (!(Mixer & (8<<c))) On = (On * NoiseHigh) >> 15;       /* noise on: high together */
#ifndef NO_SNDUNIPOLAR
		Out[c] = (4<<7) + ((Amp * (long)On) >> 7);
#else
		Out[c] = (0x080<<7) + ((Amp * ((long)On * 2 - 32768)) >> 8);
#endif
	}
	*pRight = (unsigned long)(Out[2] + Out[1]) >> 1;
	return (unsigned long)(Out[0] + Out[1]) >> 1;
}
#endif

void PSGPlay_Initialise(void)
{
	PSG_InitialiseEnvelopeShapes();
#ifndef NO_SNDENV_FIX
	PSG_RebuildEnvelopeShapes();
#endif
}

#ifdef SOUNDQ_TRACE
/* soundq (21 Oct 2026, analysis builds only): every PSG register write with
 * its NOP time and source (1 = Plus DMA) to the file in $PSGTRACE */
#include <stdio.h>
#include <stdlib.h>
int SoundQ_FromDMA;
extern unsigned long CPC_GetNopCount(void);
#endif
void PSGPlay_Write(int Register, int Data)
{
#ifdef SOUNDQ_TRACE
	{ static FILE *f; static int tried; if (!tried) { tried = 1; if (getenv("PSGTRACE")) f = fopen(getenv("PSGTRACE"), "w"); }
	  if (f) fprintf(f, "%lu %d %d %d\n", CPC_GetNopCount(), SoundQ_FromDMA, Register, Data); }
#endif
	AudioEvent_AddEventToBuffer(AUDIO_EVENT_PSG, Register, Data);
}
