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
#ifndef NO_VSYNCCUT
/* Amiga port (lag18, 5 Nov 2026): the frame ends at the CPC's VSYNC. Before, a
 * frame was 19968 cycles counted from power on, wherever that fell in the CPC's
 * own frame (60 lines after the VSYNC in Sonic GX, 253 in Ghosts 'n Goblins).
 * The host reads the joystick and shows the picture at the frame end, so where
 * the cut fell could put the game's answer to a press a whole frame later, and
 * the rows drawn after the VSYNC but before the cut belonged to the next CPC
 * frame (a seam in fast vertical scrolling).
 * Now each frame end measures how far the monitor is past its frame start (its
 * VSYNC); if the cut is off by more than VCUT_AFTER after it or VCUT_BEFORE
 * before it, twice in a row by about the same amount, the next frame is made
 * shorter (or longer, if the shift is over half a frame) once, so that the
 * frame after it ends VCUT_T cycles after the VSYNC. Frames stay 19968 cycles
 * otherwise. A game that moves its VSYNC (R7, a frame not 312 lines) is
 * followed the same way; with no VSYNC the monitor runs free at 312 lines and
 * the cut locks to that.
 * The sound is not cut at the frame end any more but on the old 19968-cycle
 * grid (AudioAt: the LocalNopCountToDate value at which the next grid point
 * falls), with the CRTC exactly where it was then, so the sound blocks and the
 * CPU and CRTC stream are the same as before, cycle for cycle; only the host's
 * frame end (the input, the picture, the 50 Hz wait) moves. The host takes the
 * blocks through CPC_AudioFrameHook (it calls CPC_UpdateAudio); NULL (a host
 * that doesn't set it): the frame end as before. NO_VSYNCCUT: as before. */
void (*CPC_AudioFrameHook)(void) = NULL;
static int AudioAt = 0;               /* > 0: the sound grid point falls in this frame at this count */
static int VcutPrevE = -1;            /* the last frame end's distance past the monitor frame start, -1 none */
static int EventAt = 0;               /* the larger of RenderOnEarlyAt and AudioAt (one test per instruction) */
unsigned long CPC_VcutShifts;         /* frames lengthened or shortened (host statistics) */
#define VCUT_T 32                     /* the frame end this many cycles after the monitor's frame start */
#define VCUT_AFTER (4 * 64)
#define VCUT_BEFORE (8 * 64)
#define EVENT_UPDATE() (EventAt = (CPC_AudioFrameHook && AudioAt > RenderOnEarlyAt) ? AudioAt : RenderOnEarlyAt)
#include "crtc.h"
static int Vcut_FrameLength(void)
{
	CRTC_INTERNAL_STATE *s = CRTC_GetInternalState();
	int e = s->Monitor_State.MonitorScanLineCount * 64 + s->Monitor_State.MonitorHorizontalCount;
	int prev = VcutPrevE, L;
	VcutPrevE = e;
	if (e < VCUT_AFTER || e > NOPS_PER_MONITOR_SCREEN - VCUT_BEFORE) return NOPS_PER_MONITOR_SCREEN;   /* at the VSYNC */
	if (prev < 0 || e - prev > 128 || prev - e > 128) return NOPS_PER_MONITOR_SCREEN;   /* not steady yet */
	L = NOPS_PER_MONITOR_SCREEN - e + VCUT_T;
	if (L < NOPS_PER_MONITOR_SCREEN / 2) L += NOPS_PER_MONITOR_SCREEN;
	VcutPrevE = -1;
	CPC_VcutShifts++;
	return L;
}
#else
#define EventAt RenderOnEarlyAt
#define EVENT_UPDATE() ((void)0)
#endif
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
#ifndef NO_VSYNCCUT
	/* VSYNCCUT: never past the sound grid point (the frame end it was written for), not the frame end */
	int lim = CPC_AudioFrameHook ? NopCountToDate - AudioAt : NopCountToDate;
	if (k > lim - CPC_IOAhead) k = lim - CPC_IOAhead;
#else
	if (k > NopCountToDate - CPC_IOAhead) k = NopCountToDate - CPC_IOAhead;
#endif
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
#ifndef NO_VSYNCCUT
	AudioAt = 0; VcutPrevE = -1; EVENT_UPDATE();   /* the grid starts here, as the frames did */
#endif
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
			while (NopCountToDate > EventAt + 1)
			{
				int k = CRTC_CyclesToEvent();
				if (k > NopCountToDate - EventAt - 1) k = NopCountToDate - EventAt - 1;
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
#ifdef Z80TRACE
		/* headless opcode-timing audit (tools/z80time): PC and NOP count before each instruction */
		{ extern void Z80Trace(void); Z80Trace(); }
#endif
#if defined(CPC_NODEBUGGER) && defined(IOT_INT_SAMPLE)
		{ extern int Z80_ExecuteStep(void); NopCount = Z80_ExecuteStep(); }
#elif defined(CPC_NODEBUGGER)
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
		if (ARN_UNLIKELY(LocalNopCountToDate<=EventAt))
		{
			int NopsToFrameEnd;
#ifndef NO_VSYNCCUT
			int crtc_done = 0, grid_now = 0, VcutL = NOPS_PER_MONITOR_SCREEN;
			if (CPC_AudioFrameHook && AudioAt > 0 && LocalNopCountToDate <= AudioAt)
			{
				/* VSYNCCUT: a sound grid point (the old frame end) inside the frame: the
				   CRTC run up to it, the sound block, then the rest of the instruction,
				   exactly as the old frame end did */
				int n = NopCount + LocalNopCountToDate - AudioAt;
				IO_AHEAD_TAKE(n);
				if (n != 0) CRTC_DoCycles(n);
				{ extern void CRTC_Flush(void); CRTC_Flush(); }
				CPC_AudioFrameHook();
				if (AudioAt - LocalNopCountToDate != 0) CRTC_DoCycles(AudioAt - LocalNopCountToDate);
				AudioAt -= NOPS_PER_MONITOR_SCREEN;
				crtc_done = 1;
				EVENT_UPDATE();
				if (LocalNopCountToDate > EventAt) { NopCountToDate = LocalNopCountToDate; continue; }
			}
#endif

			if (LocalNopCountToDate > 0)
			{
				/* Amiga port: a skipped frame before a drawn one: drawing on now */
#ifndef NO_VSYNCCUT
				if (!crtc_done)
#endif
				{ int n = NopCount; IO_AHEAD_TAKE(n); CRTC_DoCycles(n); }
				RenderOnEarlyAt = 0;
				EVENT_UPDATE();
				if (Render_IsRendererActive())
				{
					DontRender = FALSE;
					CRTC_SetRenderState(DontRender);
				}
				NopCountToDate = LocalNopCountToDate;
				continue;
			}

#ifndef NO_VSYNCCUT
			if (CPC_AudioFrameHook)
			{
				/* VSYNCCUT: the frame end is off the grid: the whole instruction is run
				   (only up to a grid point inside it, which is done below) */
				grid_now = AudioAt <= 0 && AudioAt >= LocalNopCountToDate;
				if (!crtc_done)
				{
					int n = grid_now ? NopCount + LocalNopCountToDate - AudioAt : NopCount;
					IO_AHEAD_TAKE(n);
					if (n != 0) CRTC_DoCycles(n);
				}
				{ extern void CRTC_Flush(void); CRTC_Flush(); }
				VcutL = Vcut_FrameLength();
			}
			else
#endif
			{
			NopsToFrameEnd = NopCount + LocalNopCountToDate;
			IO_AHEAD_TAKE(NopsToFrameEnd);   /* CPC_IOCatchUp stops at the frame end */

			/* update CRTC for NopCount cycles */
			if (NopsToFrameEnd!=0)
				CRTC_DoCycles(NopsToFrameEnd);
			{ extern void CRTC_Flush(void); CRTC_Flush(); }   /* Amiga port: deferred cells (crtc.c) */
			}
		
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
		
#ifndef NO_VSYNCCUT
			if (CPC_AudioFrameHook)
			{
				if (grid_now)   /* the grid point in this instruction, after the frame end */
				{
					CPC_AudioFrameHook();
					if(bWin) if (AudioAt - LocalNopCountToDate != 0)
						CRTC_DoCycles(AudioAt - LocalNopCountToDate);
					AudioAt -= NOPS_PER_MONITOR_SCREEN;
				}
				LocalNopCountToDate += VcutL;
				AudioAt += VcutL;
				EVENT_UPDATE();
			}
			else
#endif
			{
			if(bWin) if (LocalNopCountToDate!=0)
				CRTC_DoCycles(-LocalNopCountToDate);

			/* a whole screen has been timed */
			LocalNopCountToDate += NOPS_PER_MONITOR_SCREEN;
#ifndef NO_VSYNCCUT
			AudioAt = 0; VcutPrevE = -1; EVENT_UPDATE();   /* no hook: the frame end is the grid */
#endif
			}

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
#ifndef NO_VSYNCCUT
	/* lag18: the sound grid against the frame end, and the last phase reading. 1.8 saves them
	   in the ARNX chunk (SS_ArnoldExt), so a 1.7 Arnold can load the state; here they are only
	   read from a lag18 test state that had them at the end of this section. None: the frame
	   end was on the grid (a 1.7 state), unless ARNX (read after this) says otherwise. */
	if (io->load && io->pos < io->end[io->depth - 1]) { SS_V(io, AudioAt); SS_V(io, VcutPrevE); }
	else if (io->load) { AudioAt = 0; VcutPrevE = -1; }
	if (io->load) EVENT_UPDATE();
#endif
	if (io->load) {
#ifndef NO_IOTIME_FIX
		CPC_IOPre = pre; CPC_IOAhead = ahead;
#endif
		if (CurrentFrameIndex > FrameSkip) CurrentFrameIndex = 0;   /* another frame skip set now */
	}
}

/* 1.8: the ARNX chunk's section "VCUT" (savestate.c) */
void SS_ArnoldExt(SS_IO *io)
{
#ifndef NO_VSYNCCUT
	SS_V(io, AudioAt); SS_V(io, VcutPrevE);
	if (io->load) EVENT_UPDATE();
#else
	if (io->load) io->pos = io->end[io->depth - 1];   /* a build without the VSYNC cut: not used */
#endif
}
#endif
