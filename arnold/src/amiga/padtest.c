/* PadTest: shows what a joystick or pad in port 2 sends, three ways at once,
 * and guesses the pad's mode. Shell tool; Ctrl-C quits.
 *   hw   - the plain hardware read (directions, fire 1 on CIA-A, fire 2 on POTINP)
 *   ll   - lowlevel.library ReadJoyPort(1): its idea of the device and buttons
 *   cd32 - the CD32 shift-register read (7 buttons + 2 ID bits)
 * A line is printed whenever anything changes. 15 Oct 2026, for John's KTRL CD32 pad
 * (modes: CD32, single fire, dual fire, run'n'jump). */
#include <exec/types.h>
#include <hardware/custom.h>
#include <hardware/cia.h>
#include <libraries/lowlevel.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/lowlevel.h>
#include <stdio.h>

extern struct Custom custom;
extern struct CIA ciaa;
struct Library *LowLevelBase;

/* CD32 read on either port. Port 2: clock = fire (CIA-A PRA bit 7), mode = pin 5
 * (POTGO OUTRX/DATRX), data = pin 9 (POTINP DATRY bit 14). Port 1: clock = PRA bit 6,
 * mode = OUTLX/DATLX, data = DATLY bit 10. */
static unsigned cd32_read(int p2)
{
	volatile UBYTE *pra = &ciaa.ciapra, *ddra = &ciaa.ciaddra;
	UBYTE clk = p2 ? 0x80 : 0x40;
	UWORD mode = p2 ? 0x6f00 : 0xf200, data = p2 ? 0x4000 : 0x0400;
	unsigned bits = 0;
	int i, d;
	UBYTE dummy;
	Disable();
	*ddra |= clk;
	*pra &= ~clk;
	custom.potgo = mode;
	for (d = 0; d < 25; d++) dummy = *pra;
	for (i = 0; i < 9; i++) {
		for (d = 0; d < 10; d++) dummy = *pra;
		if (!(custom.potinp & data)) bits |= 1u << i;
		*pra |= clk;
		for (d = 0; d < 2; d++) dummy = *pra;
		*pra &= ~clk;
	}
	*ddra &= ~clk;
	custom.potgo = 0xff00;
	Enable();
	(void)dummy;
	return bits;
}

static void dirs(char *s, int up, int down, int left, int right)
{
	s[0] = up ? 'U' : '.'; s[1] = down ? 'D' : '.'; s[2] = left ? 'L' : '.'; s[3] = right ? 'R' : '.'; s[4] = 0;
}

static int port2 = 1;   /* PORT=1: the mouse port (joy0dat, CIA fire bit 6, POTINP bit 10) */

int main(int argc, char **argv)
{
	unsigned long last_hw = ~0UL, last_ll = ~0UL, last_cd = ~0UL;
	unsigned seen = 0, seen_last = 0;   /* hw F1, hw F2, cd32 B R Y G < > P */
	int n;
	if (argc > 1 && (argv[1][0] == 'P' || argv[1][0] == 'p') && argv[1][5] == '1') port2 = 0;
	LowLevelBase = OpenLibrary((STRPTR)"lowlevel.library", 39);
	printf("PadTest - port %d (%s). Press buttons; Ctrl-C quits. PORT=1 tests the mouse port.\n",
		port2 ? 2 : 1, port2 ? "the joystick port" : "the mouse port - unplug the mouse first");
	printf("lowlevel.library: %s\n\n", LowLevelBase ? "open" : "not found (ll column off)");
	printf("hw: dirs F1 F2(pin9) F(pin5) | ll: type dirs buttons | cd32: B R Y G < > P id | guess\n");
	for (n = 0; !(SetSignal(0, 0) & SIGBREAKF_CTRL_C); n++) {
		UWORD j = port2 ? custom.joy1dat : custom.joy0dat;
		int f1, f2, f3;
		unsigned long hw, ll = 0, cd;
		char hd[5], ld[5];
		const char *type = "-", *guess;
		custom.potgo = 0xff00;
		f1 = !(ciaa.ciapra & (port2 ? 0x80 : 0x40));
		f2 = !(custom.potinp & (port2 ? 0x4000 : 0x0400));
		f3 = !(custom.potinp & (port2 ? 0x1000 : 0x0100));   /* pin 5: AmiMSX reads fire 2 here */
		dirs(hd, ((j >> 9) ^ (j >> 8)) & 1, ((j >> 1) ^ j) & 1, (j >> 9) & 1, (j >> 1) & 1);
		hw = ((unsigned long)j << 3) | (f3 << 2) | (f1 << 1) | f2;
		if (LowLevelBase) {
			ll = ReadJoyPort(port2 ? 1 : 0);
			switch (ll & JP_TYPE_MASK) {
			case JP_TYPE_GAMECTLR: type = "CD32"; break;
			case JP_TYPE_JOYSTK: type = "joy "; break;
			case JP_TYPE_MOUSE: type = "mous"; break;
			default: type = "none"; break;
			}
		}
		dirs(ld, (ll & JPF_JOY_UP) != 0, (ll & JPF_JOY_DOWN) != 0, (ll & JPF_JOY_LEFT) != 0, (ll & JPF_JOY_RIGHT) != 0);
		cd = cd32_read(port2);
		/* a CD32 pad answers with ID bits 8th = 0 (low on pin 9 -> bit set? no: set = low) */
		if ((cd & 0x180) == 0x100) guess = "CD32 mode (answers the CD32 read)";
		else if (f2) guess = "2-button mode: fire 2 seen on pin 9";
		else if (f3) guess = "2nd button on pin 5 (AmiMSX style)";
		else guess = "joystick/1-button (or CD32 pad not answering)";
		if (f1) seen |= 1; if (f2) seen |= 2; if (f3) seen |= 0x200;
		if ((cd & 0x180) == 0x100) seen |= (cd & 0x7f) << 2;   /* only when the pad answers as CD32 */
		if (seen != seen_last) {
			static const char *nm[10] = { "fire1", "fire2(pin9)", "Blue", "Red", "Yellow", "Green", "<<", ">>", "Play", "fire(pin5)" };
			int b;
			printf("  >> seen so far:");
			for (b = 0; b < 10; b++) if (seen & (1u << b)) printf(" %s", nm[b]);
			printf("\n");
			seen_last = seen;
		}
		if (hw != last_hw || ll != last_ll || cd != last_cd) {
			printf("hw: %s %c %c %c | ll: %s %s %c%c%c%c%c%c%c | cd32: %c %c %c %c %c %c %c %u%u | %s\n",
				hd, f1 ? '1' : '.', f2 ? '2' : '.', f3 ? '5' : '.',
				type, ld,
				(ll & JPF_BUTTON_BLUE) ? 'B' : '.', (ll & JPF_BUTTON_RED) ? 'R' : '.', (ll & JPF_BUTTON_YELLOW) ? 'Y' : '.',
				(ll & JPF_BUTTON_GREEN) ? 'G' : '.', (ll & JPF_BUTTON_REVERSE) ? '<' : '.', (ll & JPF_BUTTON_FORWARD) ? '>' : '.',
				(ll & JPF_BUTTON_PLAY) ? 'P' : '.',
				(cd & 1) ? 'B' : '.', (cd & 2) ? 'R' : '.', (cd & 4) ? 'Y' : '.', (cd & 8) ? 'G' : '.',
				(cd & 16) ? '<' : '.', (cd & 32) ? '>' : '.', (cd & 64) ? 'P' : '.',
				(unsigned)((cd >> 7) & 1), (unsigned)((cd >> 8) & 1), guess);
			last_hw = hw; last_ll = ll; last_cd = cd;
		}
		Delay(2);
	}
	printf("\nBye.\n");
	if (LowLevelBase) CloseLibrary(LowLevelBase);
	return 0;
}
