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
#include "arnold.h"
#include "cpc.h"
#include "audioevent.h"
#include "host.h"
#include "z80/z80.h"
#include "fdd.h"

#define FRAME_SKIP_MIN	0
#define FRAME_SKIP_MAX	10

#ifndef CPC_NODEBUGGER
#include "debugger/gdebug.h"
#endif

BOOL bWin = TRUE;

#ifdef SHOW_TIME

/* the following are for timing the emulation */
#define FRAME_TIME_IN_MS	(1000/50)
	
#define NUM_FRAMES_TO_TIME	10
static unsigned long FrameTimesInMs[NUM_FRAMES_TO_TIME];
static unsigned long FrameIndexForFrameTimes = 0;
static unsigned long Arnold_PreviousTimeInMs = 0;
static unsigned int Arnold_PercentRelativeSpeed = 0;
#endif

/* true if not to render anything to screen buffer, FALSE to render */
static BOOL DontRender=FALSE;
/* Amiga port: automatic frame skip (14 Oct 2026). The host may set this hook;
 * at each frame end it says whether the frame after the next one should go
 * undrawn (a machine that is behind skips drawing to catch up). NULL: every
 * frame is drawn, as before.
 * Why one frame ahead: the core's own skip turns drawing back on at the frame
 * boundary, which falls mid-line; the draw position along that line and the
 * Plus soft-scroll shift register (GraphicsLong) are then stale, and the rest
 * of the line lands in the wrong place (one wrong row per drawn frame, found
 * by tools/headless_skipcheck.sh; Arnold's FrameSkip has the same flaw). So a
 * skipped frame followed by a drawn one turns drawing on RENDER_LEAD cycles
 * before its end (several whole lines, which the drawn frame then overwrites),
 * and only frames meant to be drawn are passed to the host. */
int (*CPC_SkipNextFrameHook)(void) = NULL;
#define RENDER_LEAD 1024              /* > 3 lines of the longest CRTC line (256) */
static BOOL RenderNextFrame = TRUE;   /* decided one frame ahead */
static BOOL FrameMeantToRender = TRUE;
static int RenderOnEarlyAt = 0;       /* > 0: turn drawing on this many cycles before the frame end */
/* current frame index, compared against frame skip value */
static int CurrentFrameIndex = 0;
/* frame skip value */
static int FrameSkip = FRAME_SKIP_MIN;
/* number of nops executed in this frame so far */
static int NopCountToDate = NOPS_PER_MONITOR_SCREEN;
#ifndef NO_IOTIME_FIX
/* Amiga port (Pinball Dreams, 20 Oct 2026): I/O at its own NOP within the
 * instruction (z80.h Z80_IO_AT). CPC_IOPre: NOPs of the current instruction
 * before its I/O cycle; CPC_IOAhead: NOPs of it the CRTC has already been run
 * (the main loop then runs only the rest). Never past the frame end, so the
 * frame-end path below is unchanged. */
int CPC_IOPre, CPC_IOAhead;
void CPC_IOCatchUp(void)
{
	int k = CPC_IOPre - CPC_IOAhead;
	if (k > NopCountToDate - CPC_IOAhead) k = NopCountToDate - CPC_IOAhead;
	CPC_IOPre = 0;
	if (k > 0) { CRTC_DoCycles(k); CPC_IOAhead += k; }
}
#ifndef NO_IOAHEAD_DEBT
/* rel09 (21 Oct 2026): a memory write can be timed later than the instruction
 * is long in Arnold (LD (HL),n: 2 NOPs here, written at NOP 3 as in CPCEC), so
 * the CRTC may already be past the end of the instruction. Carry that lead into
 * the next instruction instead of a negative count (which drew nothing but moved
 * the monitor counters back while HCount stayed ahead). NO_IOAHEAD_DEBT: as before. */
#ifndef NO_IOTAKE_TEST
/* speed14: the lead is rarely there (only after I/O or a late write), so test it
 * first instead of storing 0 after every instruction. NO_IOTAKE_TEST: as before. */
#define IO_AHEAD_TAKE(n) do { if (__builtin_expect(CPC_IOAhead != 0, 0)) { (n) -= CPC_IOAhead; CPC_IOAhead = 0; if (__builtin_expect((n) < 0, 0)) { CPC_IOAhead = -(n); (n) = 0; } } } while (0)
#else
#define IO_AHEAD_TAKE(n) do { (n) -= CPC_IOAhead; CPC_IOAhead = 0; if (__builtin_expect((n) < 0, 0)) { CPC_IOAhead = -(n); (n) = 0; } } while (0)
#endif
#else
#define IO_AHEAD_TAKE(n) do { (n) -= CPC_IOAhead; CPC_IOAhead = 0; } while (0)
#endif
#else
#define IO_AHEAD_TAKE(n) do { } while (0)
#endif
/* TRUE if audio is enabled, FALSE otherwise */
static BOOL AudioActiveFlag = FALSE;
/* TRUE if debugger is enabled, FALSE otherwise */
static BOOL DebuggerIsActive = FALSE;

#if 0
void	CPC_DoFrameFunc(void)
{


#ifdef AY_OUTPUT
	/* if enabled, writes PSG registers to temp file */
	YMOutput_WriteRegs();
#endif

	/* Frame Skip Control code */

	/* update frame skip */
	CurrentFrameIndex++;

	if (CurrentFrameIndex==FrameSkip+1)
	{
		CurrentFrameIndex = 0;
		
		/* is rendering possible? */
		if (Render_IsRendererActive())
		{
			/* dump whole display to screen */
			Render_DumpDisplay();
		}
	}

	/* call speed throttle function - use this to 
	throttle speed at 100% */
	Host_Throttle();


    /* disc drive light indicator*/
    if (FDD_GetLEDState(0)!=0)
    {
       Host_DoDriveLEDIndicator(0, TRUE);
    }
    else
    {
       Host_DoDriveLEDIndicator(0, FALSE);
    }

/*	Host_ProcessSystemEvents(); */


	/* dont render */
	DontRender = TRUE;
	CRTC_SetRenderState(DontRender);

	/* should we render graphics? */
	if (CurrentFrameIndex == FrameSkip)
	{
		/* is rendering possible? */
		if (Render_IsRendererActive())
		{
			/* allow it to render */
			DontRender = FALSE;
			CRTC_SetRenderState(DontRender);
		}
	}

#ifdef SHOW_TIME
	/* works out the speed of the emulation based on the previous 10 
	frames. This method stores a frame time in ms for each frame in
	a table, when 10 frame times have been recorded, it works out
	the average of those times and updates the percentage speed. */
	{
		unsigned long CurrentTimeInMs;
		unsigned long MsPassed;
		int first = Arnold_PreviousTimeInMs;

		/* get time for this frame in ms */
		CurrentTimeInMs = Host_GetCurrentTimeInMilliseconds();

		/* work out ms passed based on previous ms value */
		MsPassed = CurrentTimeInMs - Arnold_PreviousTimeInMs;
		
		/* store this time */
		Arnold_PreviousTimeInMs = CurrentTimeInMs;
		
		if (!first)
		{
			/* store this frame time */
			FrameTimesInMs[FrameIndexForFrameTimes] = MsPassed;
			FrameIndexForFrameTimes++;

			if (FrameIndexForFrameTimes == NUM_FRAMES_TO_TIME)
			{
				int i;
				unsigned long AverageTime = 0;

				/* recalc relative speed */
				FrameIndexForFrameTimes = 0;

				for (i=0; i<NUM_FRAMES_TO_TIME; i++)
				{
					/* add on time for this frame */
					AverageTime+=FrameTimesInMs[i];
				}

				AverageTime = AverageTime/NUM_FRAMES_TO_TIME;

				Arnold_PercentRelativeSpeed = (FRAME_TIME_IN_MS*100)/AverageTime;
			}
		}
	}
#endif

}
#endif

#ifdef SHOW_TIME
unsigned long CPCEmulation_GetPercentRelativeSpeed(void)
{
	return Arnold_PercentRelativeSpeed;
}
#endif


/* set frame skip for emulation */
void	CPC_SetFrameSkip(int FrameSkipCount)
{
	if (FrameSkipCount<FRAME_SKIP_MIN)
		FrameSkipCount = FRAME_SKIP_MIN;

	if (FrameSkipCount>FRAME_SKIP_MAX)
		FrameSkipCount = FRAME_SKIP_MAX;

	FrameSkip = FrameSkipCount;

	CurrentFrameIndex = 0;
	DontRender = FALSE;

}

int CPC_GetFrameSkip(void)
{
	return FrameSkip;
}

BOOL	CPC_IsAudioActive(void)
{
	return AudioActiveFlag;
}

void	CPC_SetAudioActive(BOOL State)
{
	if ((AudioActiveFlag==FALSE) && (State==TRUE))
	{
		/* setup for audio playback */

		/* is host system able to playback sound? */
		if (Host_AudioPlaybackPossible())
		{
			/* yes it is */
			SOUND_PLAYBACK_FORMAT *pSoundPlaybackFormat;

			/* get playback format */
			pSoundPlaybackFormat = Host_GetSoundPlaybackFormat();

			if (pSoundPlaybackFormat!=NULL)
			{
				/* audio was previously disabled, but we want it to be enabled */
				AudioEvent_SetFormat(pSoundPlaybackFormat->Frequency, pSoundPlaybackFormat->BitsPerSample, pSoundPlaybackFormat->NumberOfChannels);
	
				/* playing audio */
				AudioActiveFlag = TRUE;
			}
		}
		else
		{
			AudioActiveFlag = FALSE;
		}

	}

	if ((AudioActiveFlag==TRUE) && (State==FALSE))
	{
		/* audio was previously enabled, but we want it to be disabled */

		/* stop audio playback */
	}
}

/* this is called after every Z80 opcode executed */
/*void Z80_OpcodeFunc(int NopCount)
{
}
*/
/* initialise default CPC Emulation settings */
void	CPCEmulation_InitialiseDefaultSetup(void)
{
	/* set initial CRTC type */
	CPC_SetCRTCType(0);

	/* set initial CPC type */
//	CPC_SetCPCType(CPC_TYPE_CPC6128);

	/* set initial printer output method */
//	Printer_SetOutputMethod(PRINTER_OUTPUT_TO_DIGIBLASTER);

	/* set monitor to colour */
    CPC_SetMonitorType(CPC_MONITOR_COLOUR);

	/* set brightness to max */
/*   CPC_SetMonitorBrightness(MONITOR_BRIGHTNESS_MAX); */
}

void	CPC_ResetTiming(void)
{
	NopCountToDate = NOPS_PER_MONITOR_SCREEN;
}


extern unsigned char *pAudioBuffer;
extern unsigned int AudioBufferSize;
#if 0
void	CPC_UpdateAudio(void)
{
	int CPCNopCount = CPC_GetNopCount();
	int NopsReached = CPCNopCount;

	if (AudioActiveFlag)
	{
		/* update sound buffer with events in audio events buffer */
		AudioEvent_TraverseAudioEventsAndBuildSampleData();
	
		/* convert PSG vol/Digiblaster data to sample data */
		AudioEvent_ConvertToOutputFormat();
	}

	/* restart buffer ready to fill with new data */
	AudioEvent_RestartEventBuffer();
}
#endif


extern unsigned char *pAudioBuffer;
extern unsigned int AudioBufferSize;

void	CPC_UpdateAudio(void)
{
	if (AudioActiveFlag)
	{
		Digiblaster_EndFrame();
	}

	{
		int CPCNopCount = CPC_GetNopCount();
		int NopsReached = CPCNopCount;

		if (AudioActiveFlag)
		{
			/* update sound buffer with events in audio events buffer */
			NopsReached = AudioEvent_TraverseAudioEventsAndBuildSampleData(CPCNopCount,19968);
		}

		/* restart buffer ready to fill with new data */
		AudioEvent_RestartEventBuffer(NopsReached);
	}
}

void	CPCEmulation_EnableDebugger(BOOL State)
{
	DebuggerIsActive = State;
}

extern BOOL DoNotScanKeyboard;
extern int Z80_HaltHit;
extern int Z80_HaltIdle(void);
extern int Z80_InterruptRaised(void);
extern void Z80_HaltSteps(int Count);
extern int CRTC_CyclesToEvent(void);
//extern BOOL ConsoleBreak;

#include "hint.h"
void	CPCEmulation_Run(void)
{
	BOOL doBreak = FALSE;

	while (!doBreak)
	{
		int NopCount;
		int LocalNopCountToDate;

#if defined(CPC_NODEBUGGER) && !defined(NO_F)
		/* Amiga port: a halted Z80 only burns cycles until the next CRTC event
		   (the only place interrupts are raised), so do them as one batch.
		   Stops one cycle short of the frame end so that path is unchanged. */
		if (ARN_UNLIKELY(Z80_HaltHit) && (Z80_HaltHit = 0, Z80_HaltIdle()))
		{
			/* batch to each event cycle in turn; an event that raises no
			   interrupt leaves the Z80 halting, so carry on to the next one */
			int done = 0;
			while (NopCountToDate > RenderOnEarlyAt + 1)
			{
				int k = CRTC_CyclesToEvent();
				if (k > NopCountToDate - RenderOnEarlyAt - 1) k = NopCountToDate - RenderOnEarlyAt - 1;
				Z80_HaltSteps(k);
				CPC_UpdateNopCount(k);
				NopCountToDate -= k;
				CRTC_DoCycles(k);
				done = 1;
				if (Z80_InterruptRaised()) break;
			}
			if (done) continue;
		}
#endif
		/* execute the instruction */
#ifdef CPC_NODEBUGGER
		NopCount = Z80_ExecuteInstruction(); 
#else
		NopCount = Debugger_Execute(); 
#endif
		/* update CPC nop count - used for other hardware */
#ifndef NO_HINTS
		{ extern unsigned long CPC_NopCount; CPC_NopCount += NopCount; }   /* = CPC_UpdateNopCount, no call */
#else
		CPC_UpdateNopCount(NopCount);
#endif

		LocalNopCountToDate = NopCountToDate - NopCount;

		/* nop counter is used to define when we render the whole display */
		if (ARN_UNLIKELY(LocalNopCountToDate<=RenderOnEarlyAt))
		{
			int NopsToFrameEnd;

			if (LocalNopCountToDate > 0)
			{
				/* Amiga port: a skipped frame before a drawn one: drawing on now */
				{ int n = NopCount; IO_AHEAD_TAKE(n); CRTC_DoCycles(n); }
				RenderOnEarlyAt = 0;
				if (Render_IsRendererActive())
				{
					DontRender = FALSE;
					CRTC_SetRenderState(DontRender);
				}
				NopCountToDate = LocalNopCountToDate;
				continue;
			}

			NopsToFrameEnd = NopCount + LocalNopCountToDate;
			IO_AHEAD_TAKE(NopsToFrameEnd);   /* CPC_IOCatchUp stops at the frame end */

			/* update CRTC for NopCount cycles */
			if (NopsToFrameEnd!=0)
				CRTC_DoCycles(NopsToFrameEnd);	
			{ extern void CRTC_Flush(void); CRTC_Flush(); }   /* Amiga port: deferred cells (crtc.c) */
		
			/* execute functions when a frame has been completed */
			/* Frame Skip Control code */

			/* update frame skip */
			CurrentFrameIndex++;

			if(bWin) if (CurrentFrameIndex==FrameSkip+1)
			{
				CurrentFrameIndex = 0;
				
				/* is rendering possible? */
				if (Render_IsRendererActive() && FrameMeantToRender)
				{
					/* dump whole display to screen */
					Render_DumpDisplay();
				}
			}

			/* call speed throttle function - use this to 
			throttle speed at 100% */
			Host_Throttle();

         if (bWin)
         {
			 doBreak = Host_ProcessSystemEvents(); 


			/* dont render */
			DontRender = TRUE;
			CRTC_SetRenderState(DontRender);

			/* should we render graphics? */
			FrameMeantToRender = FALSE;
			if (CurrentFrameIndex == FrameSkip && RenderNextFrame)
			{
				/* is rendering possible? */
				if (Render_IsRendererActive())
				{
					/* allow it to render */
					DontRender = FALSE;
					CRTC_SetRenderState(DontRender);
					FrameMeantToRender = TRUE;
				}
			}
			/* Amiga port: decide the frame after this one now */
			RenderNextFrame = !(CPC_SkipNextFrameHook && CPC_SkipNextFrameHook());
			RenderOnEarlyAt = (FrameSkip == 0 && DontRender && RenderNextFrame) ? RENDER_LEAD : 0;
		}
		 else
		 {
//			 doBreak = ConsoleBreak;
		 }
	#ifdef SHOW_TIME
			/* works out the speed of the emulation based on the previous 10 
			frames. This method stores a frame time in ms for each frame in
			a table, when 10 frame times have been recorded, it works out
			the average of those times and updates the percentage speed. */
			{
				unsigned long CurrentTimeInMs;
				unsigned long MsPassed;
				int first = Arnold_PreviousTimeInMs;

				/* get time for this frame in ms */
				CurrentTimeInMs = Host_GetCurrentTimeInMilliseconds();

				/* work out ms passed based on previous ms value */
				MsPassed = CurrentTimeInMs - Arnold_PreviousTimeInMs;
				
				/* store this time */
				Arnold_PreviousTimeInMs = CurrentTimeInMs;
				
				if (!first)
				{
					/* store this frame time */
					FrameTimesInMs[FrameIndexForFrameTimes] = MsPassed;
					FrameIndexForFrameTimes++;

					if (FrameIndexForFrameTimes == NUM_FRAMES_TO_TIME)
					{
						int i;
						unsigned long AverageTime = 0;

						/* recalc relative speed */
						FrameIndexForFrameTimes = 0;

						for (i=0; i<NUM_FRAMES_TO_TIME; i++)
						{
							/* add on time for this frame */
							AverageTime+=FrameTimesInMs[i];
						}

						AverageTime = AverageTime/NUM_FRAMES_TO_TIME;

						Arnold_PercentRelativeSpeed = (FRAME_TIME_IN_MS*100)/AverageTime;
					}
				}
			}
	#endif
/*			CPC_DoFrameFunc(); */
		
			if(bWin) if (LocalNopCountToDate!=0)
				CRTC_DoCycles(-LocalNopCountToDate);
		
			/* a whole screen has been timed */
			LocalNopCountToDate += NOPS_PER_MONITOR_SCREEN;

		}
		else //if (bWin) this doesn't work :(
		{
			/* update CRTC for NopCount cycles */
#if !defined(NO_DEFER) && !defined(NO_E)
			/* Amiga port: quiet cycles short of the next CRTC event are only
			 * counted here and drawn later (crtc.c CRTC_Pending) */
			extern int CRTC_RunLeft, CRTC_Pending, ASIC_DMA_IntPending;
#endif
			IO_AHEAD_TAKE(NopCount);   /* NopCount is not used again below */
#if !defined(NO_DEFER) && !defined(NO_E)
			if (ARN_LIKELY(NopCount < CRTC_RunLeft && !ASIC_DMA_IntPending))
			{
				CRTC_RunLeft -= NopCount;
				CRTC_Pending += NopCount;
			}
			else
#endif
			CRTC_DoCycles(NopCount);
		}

		NopCountToDate = LocalNopCountToDate;
	}
}


void	CPCEmulation_Finish(void)
{
 AudioActiveFlag = FALSE; /* TROELS */
	CPC_Finish();
	
	Render_Finish();

#ifndef CPC_NODEBUGGER
	Debug_Finish();
#endif
}	

BOOL	CPCEmulation_CheckEndianness(void)
{
	BOOL EndianCorrect;
	unsigned long TestLong = 0;
	unsigned char *pLongPtr = (unsigned char *)&TestLong;

	pLongPtr[0] = 1;

#ifdef CPC_LSB_FIRST
	EndianCorrect = FALSE;
	if (TestLong == 1)
	{	
		/* on little endian, long = 1 */
		EndianCorrect = TRUE;
	}
#else
	EndianCorrect = TRUE;
	if (TestLong == 1)
	{
		/* on little endian, long = 1 */
		EndianCorrect = FALSE;
	}
#endif
	return EndianCorrect;
}

#ifndef NO_SAVESTATE
/* Amiga port (save states, 28 Oct 2026): the main loop's state between two
 * CPCEmulation_Run calls (cpc/savestate.c). FrameSkip and the skip hook are the
 * host's; the I/O lead is stored as 0 in a NO_IOTIME_FIX build (same stream). */
#include "savestate.h"
void SS_Arnold(SS_IO *io)
{
	int pre = 0, ahead = 0;
#ifndef NO_IOTIME_FIX
	pre = CPC_IOPre; ahead = CPC_IOAhead;
#endif
	SS_V(io, NopCountToDate); SS_V(io, RenderOnEarlyAt); SS_V(io, DontRender);
	SS_V(io, FrameMeantToRender); SS_V(io, RenderNextFrame); SS_V(io, CurrentFrameIndex);
	SS_V(io, pre); SS_V(io, ahead);
	if (io->load) {
#ifndef NO_IOTIME_FIX
		CPC_IOPre = pre; CPC_IOAhead = ahead;
#endif
		if (CurrentFrameIndex > FrameSkip) CurrentFrameIndex = 0;   /* another frame skip set now */
	}
}
#endif
