/* Arnold on AmigaOS: the joystick port (hardware registers, a CD32 pad read by
 * hand) and the pads lowlevel.library reports on ports 2-3. Split off amiga_sys.c
 * (split16, 30 Oct 2026). */
#include <exec/types.h>
#include <hardware/custom.h>
#include <hardware/cia.h>
#include <proto/exec.h>
#include "amiga_sys.h"

extern struct Custom custom;
/* The NDK's struct Custom isn't volatile: at -O2 gcc dropped the 0x6f00 POTGO
 * write (a later 0xff00 overwrites it) and read POTINP once for all nine bits,
 * so the pad never entered CD32 mode (found 7 Nov 2026). Every chip register
 * goes through this. */
#define CUSTOM ((volatile struct Custom *)&custom)
extern struct CIA ciaa;

/* Joystick: the hardware port 2 (joy1dat, CIA-A fire, POTINP fire 2) as
 * AmiMSX reads it, OR'd with lowlevel.library's view of ports 0-3 (14 Oct
 * 2026: a USB pad under Poseidon only shows up through lowlevel.library's
 * joystick-port emulation, not in the hardware registers). Port 0 is skipped
 * while it holds the mouse. Red = fire 1, blue (or any other button) = fire 2. */
#include <libraries/lowlevel.h>
#include <proto/lowlevel.h>
struct Library *LowLevelBase;
static int lowlevel_tried;

static ULONG joy_last_raw[4];
ULONG Sys_JoyRaw(int port) { return port >= 0 && port < 4 ? joy_last_raw[port] : 0; }

/* CD32 pad on port 2, read by us (15 Oct 2026). On the A1200 PiStorm,
 * lowlevel.library saw John's KTRL CD32 pad as a game controller but only
 * ever reported the red button: on a CPU that fast its shift-register reads
 * outrun the pad. This is the usual routine games use (cf. Codetapper's
 * CD32 pad notes): pin 5 low selects CD32 mode, pin 6 (fire) becomes the
 * clock, and the 7 buttons come out one by one on pin 9 (POTINP bit 14,
 * low = pressed). Each step waits on CIA reads, which run at real Amiga
 * speed whatever the CPU. Order: blue, red, yellow, green, forward, reverse,
 * play. Returns the 7 button bits (bit set = pressed). */
static unsigned read_cd32_buttons(void)
{
	/* Returns 9 bits, set = low on pin 9: the 7 buttons, then the pad's ID
	 * bits (a CD32 pad gives 8th = 0, 9th = 1 here). As AmiMSX does it
	 * (John's decision, 17 Oct 2026): pin 6 is driven HIGH as an output
	 * before pin 5 falls, otherwise a held red button gives a spurious clock;
	 * the edges are timed with CIA reads, which run at real Amiga speed
	 * whatever the CPU. Order: blue, red, yellow, green, forward, reverse,
	 * play. */
	volatile UBYTE *pra = &ciaa.ciapra, *ddra = &ciaa.ciaddra;
	unsigned bits = 0;
	int i, d;
	UBYTE dummy;
	Disable();
	*pra |= 0x80;                  /* clock high first... */
	*ddra |= 0x80;                 /* ...then port 2 fire line as an output */
	for (d = 0; d < 4; d++) dummy = *pra;
	CUSTOM->potgo = 0x6f00;        /* port 2: pin 5 output low (CD32 mode), pin 9 input */
	for (d = 0; d < 25; d++) dummy = *pra;
	for (i = 0; i < 9; i++) {
		for (d = 0; d < 10; d++) dummy = *pra;
		if (!(CUSTOM->potinp & 0x4000)) bits |= 1u << i;
		*pra &= ~0x80;             /* clock pulse: next bit */
		for (d = 0; d < 4; d++) dummy = *pra;
		*pra |= 0x80;
	}
	*ddra &= ~0x80;                /* fire line back to input */
	CUSTOM->potgo = 0xff00;        /* pin 5 high again: 2-button mode */
	Enable();
	(void)dummy;
	return bits;
}

/* Joystick port 2 (lowlevel.library port 1). No auto-detection (John's
 * decision, 17 Oct 2026, after AmiMSX: lowlevel.library's CD32 detection was
 * wrong for his adapters and even for a plain joystick, and a plain joystick
 * read as a CD32 pad has its fire line driven as a clock, so nothing works).
 * - Sys_PadType 0, standard: the hardware registers as AmiMSX reads them
 *   (joy1dat, CIA-A fire, POTINP fire 2 on pin 9 or pin 5). Pin 6 is never
 *   driven, so a plain joystick sees no probe at all.
 * - Sys_PadType 1, CD32 pad: directions from joy1dat, the buttons from
 *   read_cd32_buttons(), no ID check. Red = fire 1; blue, green, yellow = fire 2.
 * Game controllers lowlevel.library reports on ports 2-3 (USB pads under
 * Poseidon) are OR'd in either way. */
int Sys_PadType;                    /* 0 standard, 1 CD32 pad (menu / PAD= / prefs pad=) */
int Sys_NoCD32;                     /* NOCD32: kept for old scripts, same as PAD=0 */
static int pad_logged = -1;
/* 1.8 (8 Nov 2026): the two ReadJoyPort calls cost about 0.4 ms on the PiStorm A1200 (measured, with
 * Poseidon). Sys_JoyPortOnly (the read again at the game's read): port 2's hardware only, with the
 * USB pads' bits from the frame start's read (ll_bits); USB pads only change at Poseidon's own rate. */
int Sys_JoyPortOnly;
static unsigned ll_bits;
unsigned Sys_ReadJoystick(void)
{
	unsigned m = 0;
	int port;
	UWORD j = CUSTOM->joy1dat;
	if (!lowlevel_tried) {
		lowlevel_tried = 1;
		LowLevelBase = OpenLibrary((STRPTR)"lowlevel.library", 39);
		Sys_Log("joystick: lowlevel.library %s", LowLevelBase ? "open (USB pads on ports 2-3)" : "not available");
	}
	if (Sys_NoCD32) Sys_PadType = 0;
	if (pad_logged != Sys_PadType) {
		pad_logged = Sys_PadType;
		Sys_Log("joystick: port 2 read as %s", Sys_PadType ? "a CD32 pad" : "a standard 2-button joystick");
	}
	if (j & 0x0002) m |= SYS_JOY_RIGHT;
	if (j & 0x0200) m |= SYS_JOY_LEFT;
	if (((j >> 1) ^ j) & 0x0001) m |= SYS_JOY_DOWN;
	if (((j >> 9) ^ (j >> 8)) & 0x0001) m |= SYS_JOY_UP;
	if (Sys_PadType) {
		unsigned b = read_cd32_buttons();
		joy_last_raw[1] = b;
#ifndef NO_CD32PLAIN
		/* 1.8 (emu-dev-docs cross-review, as AmiMSX): a plain pad answers every clock with the
		 * same level, so fire 2 held reads as all 7 buttons (red = fire 1 too). All 7 at once:
		 * taken as a plain pad, fire 2 only (pin 9; its fire 1 can't be read while pin 6 is the
		 * clock). A real CD32 pad with all 7 held reads the same way. */
		if ((b & 0x7F) == 0x7F) m |= SYS_JOY_FIRE2;
		else
#endif
		{
			if (b & 0x02) m |= SYS_JOY_FIRE1;                    /* red */
			if (b & (0x01 | 0x04 | 0x08)) m |= SYS_JOY_FIRE2;   /* blue, yellow, green */
		}
	} else {
		if (!(ciaa.ciapra & 0x80)) m |= SYS_JOY_FIRE1;
		CUSTOM->potgo = 0xff00;
		/* fire 2 on pin 9 (Amiga standard) or pin 5 (as AmiMSX reads it; some 2-button pads) */
		if (!(CUSTOM->potinp & 0x4000) || !(CUSTOM->potinp & 0x1000)) m |= SYS_JOY_FIRE2;
	}
	if (LowLevelBase && !Sys_JoyPortOnly) {
		unsigned long ll0 = Sys_Micros();
		unsigned ll = 0;
		for (port = 2; port < 4; port++) {
			ULONG v = ReadJoyPort(port);
			ULONG type = v & JP_TYPE_MASK;
			if (v != joy_last_raw[port]) joy_last_raw[port] = v;
			if (type != JP_TYPE_GAMECTLR && type != JP_TYPE_JOYSTK) continue;
			if (v & JPF_JOY_UP) ll |= SYS_JOY_UP;
			if (v & JPF_JOY_DOWN) ll |= SYS_JOY_DOWN;
			if (v & JPF_JOY_LEFT) ll |= SYS_JOY_LEFT;
			if (v & JPF_JOY_RIGHT) ll |= SYS_JOY_RIGHT;
			if (v & JPF_BUTTON_RED) ll |= SYS_JOY_FIRE1;
			if (v & (JPF_BUTTON_BLUE | JPF_BUTTON_GREEN | JPF_BUTTON_YELLOW)) ll |= SYS_JOY_FIRE2;
		}
		ll_bits = ll;
		Sys_Perf.jr_ll_sum += Sys_Micros() - ll0;
		Sys_Perf.jr_ll_n++;
	}
	return m | ll_bits;
}
