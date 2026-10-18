/* Arnold on AmigaOS: the OS-facing half of the host. No Arnold headers here
 * (cpcglob.h has its own BOOL, which clashes with exec/types.h), so
 * host_amiga.c talks to the Amiga through this small interface only. */
#ifndef AMIGA_SYS_H
#define AMIGA_SYS_H

/* Display: an 8-plane PAL screen, lores (cw = 384) or hires (cw = 768),
 * double-buffered. The chunky buffer the core renders into is cw x ch; the
 * screen may be smaller (overscan limits) and shows the centred part of it. */
int  Sys_OpenDisplay(int cw, int ch, int hires);
void Sys_CloseDisplay(void);
void Sys_SetPaletteEntry(int index, unsigned char r, unsigned char g, unsigned char b);
void Sys_ShowFrame(const unsigned char *chunky, int pitch);  /* c2p changed lines, flip */
void Sys_MarkLineDirty(int y);  /* chunky line y changed: convert it into every buffer */
long Sys_WaitFrame(void);                                    /* VBlank pacing; returns us behind schedule */

/* Input. Sys_Poll drains Intuition events; returns 1 if the user asked to quit. */
int  Sys_Poll(void);
void Sys_RequestQuit(void);
int  Sys_GetKey(int *rawcode, int *down, int *qualifier);  /* 1 while key events are queued */
#define SYS_JOY_UP    0x01
#define SYS_JOY_DOWN  0x02
#define SYS_JOY_LEFT  0x04
#define SYS_JOY_RIGHT 0x08
#define SYS_JOY_FIRE1 0x10
#define SYS_JOY_FIRE2 0x20
unsigned Sys_ReadJoystick(void);   /* port 2 (the joystick port) */
unsigned long Sys_JoyRaw(int port); /* last lowlevel.library ReadJoyPort value (KEYLOG) */

/* Sound: Paula, channels 0 (left) and 1 (right), 8-bit, one block per frame.
 * Sys_AudioOpen allocates the channels through audio.device; period is the
 * Paula period, samples the block length (even). Sys_AudioQueue takes a frame
 * of the core's 16-bit native-order stereo samples; a VBlank interrupt hands
 * the queued blocks to Paula one per frame. */
int  Sys_AudioOpen(int period, int samples);
void Sys_AudioClose(void);
void Sys_AudioQueue(const unsigned char *s16stereo, int samples);

void Sys_AudioPause(void);           /* DMA off; the next Sys_AudioQueue restarts it */

/* The menu screen (amiga_menu.c). Sys_Menu edits m and returns an action. */
#define SYS_PATH 256
struct SysMenu {
	char cart[SYS_PATH], diska[SYS_PATH], diskb[SYS_PATH];
	int sound, hires;
	int skip, refresh;       /* frame skip (max in a row, 0 off) and fixed refresh (1 = every frame) */
	int joykeys;             /* keyboard as joystick: 0 off, 1 cursors, 2 QAOP */
	int pad;                 /* port 2 pad: 0 standard 2-button joystick, 1 CD32 pad */
	int can_continue;        /* a machine is running (Help brought us here) */
	char prefs[SYS_PATH];    /* shown in the status line */
	int game_prefs;          /* the cartridge has its own <file>.prefs (shown) */
	char status[80];         /* one-off status line for the next menu (cleared) */
};
enum { SYS_MENU_QUIT = 0, SYS_MENU_POWER, SYS_MENU_CONTINUE, SYS_MENU_RESET, SYS_MENU_SAVE,
       SYS_MENU_SAVEGAME, SYS_MENU_FORGETGAME, SYS_MENU_SHOT };
int  Sys_Menu(struct SysMenu *m);
void Sys_MenuError(const char *text);
void Sys_ShowPicture(void *screen, const char *name);   /* amiga_iff.c: modal IFF viewer */

/* Workbench start (amiga_wb.c): fills st from the icon's ToolTypes and a
 * project icon; returns 1 when started from Workbench. */
struct SysStart { char prefs[SYS_PATH], media[SYS_PATH]; int menu, hires, nosound, skip, refresh, joykeys, pad; };   /* skip -1, refresh 0, joykeys -1: not given */
int  Sys_WBStart(int argc, char **argv, struct SysStart *st);

unsigned long Sys_Millis(void);

/* Performance counters (timer.device E-clock): time in c2p (conversion plus the
 * chip RAM writes), time waiting for flips/VBlank, lines converted. */
typedef struct { unsigned long c2p_us, wait_us, c2p_lines, c2p_frames, line_bytes,
                 wait_safe_us, wait_disp_us, wait_throttle_us, throttle_calls, late_frames,
                 pal_us, palette_loads, skipped_frames,
                 aud_underruns, aud_drops; } SysPerf;
extern SysPerf Sys_Perf;
unsigned long Sys_Micros(void);
#include <stdio.h>
void Sys_Bench(FILE *f);   /* BENCH argument: CPU, memory and chip RAM timings */
void Sys_Message(const char *text);  /* requester */
/* Diagnostic log, PROGDIR:Arnold.log (T: if that fails): one line per stage of
 * the start-up and every error, flushed at once, so a failure on real
 * hardware leaves a trace. Sys_LogOpen truncates it; Sys_Log appends. */
void Sys_LogOpen(void);
void Sys_Heartbeat(long frame, long behind_us);   /* one log line about once a second */
extern int Sys_PadType;             /* 0 standard 2-button joystick, 1 CD32 pad (no auto-detection) */
extern int Sys_NoCD32;               /* NOCD32: port 2 as a plain 2-button joystick */
extern int Sys_PalAll;              /* PALALL: reload the whole palette every frame */
extern int Sys_AllLines;            /* ALLLINES: convert every line every frame (no dirty-line tracking) */
extern int Sys_SafeFlip;            /* SAFEFLIP: wait for the previous flip's messages before the next */
extern int Sys_Osd, Sys_OsdSkip;     /* OSD: test-mode label in the picture's corner; skip value it shows */
#define SYS_OSD_PEN 255             /* palette entry the host keeps for the label (yellow) */
void Sys_MakeDir(const char *path);   /* create a drawer if it is missing (SNAPDIR) */
void Sys_LogStart(int argc);        /* the first line: machine, memory, start mode */
void Sys_Log(const char *fmt, ...);
void Sys_LogClose(void);

#endif
