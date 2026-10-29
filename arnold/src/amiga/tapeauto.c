/* Auto-start tapes (1.5, 28 Oct 2026): see tapeauto.h.
 * After Power ON (or a Reset with the tape at its start) the host calls
 * TapeAuto_Start; TapeAuto_Frame then types, frame by frame, what the user
 * would: on a 6128 Plus with the system cartridge f1 (BASIC), on a 664, 6128
 * or Plus |TAPE and Return, then RUN" and Return, and a little later Space for
 * the firmware's "Press PLAY then any key:". The keys go into the CPC's
 * keyboard matrix at frame ends, exactly as a key script's do, so the machine
 * runs as if the user had typed them (the emulation itself is not touched).
 * Timing: power on is a power cycle, so the machine reaches BASIC's Ready at the
 * same frame every time; the first key comes well after that (measured with the
 * headless host, tools/v15/boot_margin.sh: typing works from frame 40 on the
 * 464, 664 and 6128, which get it at 100; the Plus menu takes f1 from frame 40,
 * it gets it at 150, and BASIC types from 100 frames after f1, it gets 150).
 * Each key is held 4 frames and let go
 * for 4 (the firmware needs a release of at least one scan, and two for the
 * same key twice). Plain C, no OS calls. */
#include "../cpc/cpc.h"
#include "tapeauto.h"

#define TA_HOLD 4
#define TA_STEP 8                     /* frames a character: TA_HOLD down, then up */
#define TA_F1_AT 150                  /* Plus: f1 at the system cartridge's menu */
#define TA_PLAY_WAIT 50               /* after the last Return: Space for "Press PLAY then any key" */
static int ta_kind;
static long ta_t;                     /* frames since TapeAuto_Start */

static const char *ta_text(int kind) { return kind == TAPEAUTO_464 ? "RUN\"\n" : "|TAPE\nRUN\"\n"; }
static long ta_first(int kind) { return kind == TAPEAUTO_PLUS ? 300 : 100; }
static int ta_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* the CPC key for a character, and whether it needs Shift */
static int ta_key(char c, int *shift)
{
	*shift = 0;
	switch (c) {
	case '|': *shift = 1; return CPC_KEY_AT;
	case '"': *shift = 1; return CPC_KEY_2;
	case '\n': return CPC_KEY_RETURN;
	case 'A': return CPC_KEY_A;
	case 'E': return CPC_KEY_E;
	case 'N': return CPC_KEY_N;
	case 'P': return CPC_KEY_P;
	case 'R': return CPC_KEY_R;
	case 'T': return CPC_KEY_T;
	case 'U': return CPC_KEY_U;
	}
	return CPC_KEY_SPACE;
}

void TapeAuto_Start(int kind) { ta_kind = kind; ta_t = 0; }
void TapeAuto_Stop(void) { ta_kind = TAPEAUTO_OFF; ta_t = 0; }
int  TapeAuto_Active(void) { return ta_kind != TAPEAUTO_OFF; }

void TapeAuto_Frame(void (*setkey)(int key, int down))
{
	const char *s;
	long first, i, ph, end;
	int n, key, shift;
	if (ta_kind == TAPEAUTO_OFF) return;
	ta_t++;
	if (ta_kind == TAPEAUTO_PLUS) {
		if (ta_t == TA_F1_AT) setkey(CPC_KEY_F1, 1);
		if (ta_t == TA_F1_AT + TA_HOLD) setkey(CPC_KEY_F1, 0);
	}
	s = ta_text(ta_kind); n = ta_len(s); first = ta_first(ta_kind);
	if (ta_t >= first && ta_t < first + (long)n * TA_STEP) {
		i = (ta_t - first) / TA_STEP; ph = (ta_t - first) % TA_STEP;
		key = ta_key(s[i], &shift);
		if (ph == 0) { if (shift) setkey(CPC_KEY_SHIFT, 1); setkey(key, 1); }
		if (ph == TA_HOLD) { setkey(key, 0); if (shift) setkey(CPC_KEY_SHIFT, 0); }
	}
	end = first + (long)n * TA_STEP + TA_PLAY_WAIT;
	if (ta_t == end) setkey(CPC_KEY_SPACE, 1);
	if (ta_t == end + TA_HOLD) { setkey(CPC_KEY_SPACE, 0); ta_kind = TAPEAUTO_OFF; ta_t = 0; }   /* done (as Stop: a state round trip stays byte-identical) */
}

void TapeAuto_GetState(long *v) { v[0] = ta_kind; v[1] = ta_t; }
void TapeAuto_SetState(const long *v)
{
	ta_kind = (v[0] >= TAPEAUTO_464 && v[0] <= TAPEAUTO_PLUS && v[1] >= 0) ? (int)v[0] : TAPEAUTO_OFF;
	ta_t = ta_kind ? v[1] : 0;
}
