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
long Sys_WaitFrame(void);                                    /* frame pacing; returns us behind schedule */
void Sys_SetPacing(int timer, unsigned long frame_us);       /* 0: one frame a VBlank; 1: E-clock timer, frame_us apart */
/* Video mode of both screens: 0 Auto (default monitor, as Workbench), 1 PAL, 2 NTSC */
extern int Sys_VideoMode;
unsigned long Sys_ModeID(unsigned long key);                 /* LORES_KEY / HIRES_KEY with the chosen monitor */
extern unsigned long Sys_DisplayFrameUs;                     /* measured VBlank interval of the last emulation screen */
int  Sys_Display50Hz(void);

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
int  Sys_AudioOpen(int period, int samples, int audint);   /* audint: fed by Paula's own interrupt, not the VBlank */
unsigned long Sys_ChipClock(void);   /* Paula clock, Hz (PAL 3546895, NTSC 3579545) */
void Sys_AudioClose(void);
void Sys_AudioQueue(const unsigned char *s16stereo, int samples);

void Sys_AudioPause(void);           /* DMA off; the next Sys_AudioQueue restarts it */

/* The menu screen (amiga_menu.c). Sys_Menu edits m and returns an action. */
#define SYS_PATH 256
struct SysMenu {
	char cart[SYS_PATH], diska[SYS_PATH], diskb[SYS_PATH];
	int sound, hires;
	int tall;                /* full-height screen: all 272 CPC lines (PAL overscan), for games drawn into the top/bottom border */
	int skip, refresh;       /* frame skip (max in a row, 0 off) and fixed refresh (1 = every frame) */
	int joykeys;             /* keyboard as joystick: 0 off, 1 cursors, 2 QAOP */
	int pad;                 /* port 2 pad: 0 standard 2-button joystick, 1 CD32 pad */
	int speed;               /* game speed: 0 the CPC's 50 Hz (E-clock timer off PAL), 1 follow the display */
	int video;               /* video mode of both screens: 0 Auto (as Workbench), 1 PAL, 2 NTSC */
	int model;               /* SYS_MODEL_*: takes effect on Power ON */
	int running_model;       /* the model of the machine that is running (-1 none) */
	int can_continue;        /* a machine is running (Help brought us here) */
	char prefs[SYS_PATH];    /* shown in the status line */
	int game_prefs;          /* the cartridge has its own <file>.prefs (shown) */
	char status[80];         /* one-off status line for the next menu (cleared) */
};
enum { SYS_MODEL_464 = 0, SYS_MODEL_664, SYS_MODEL_6128, SYS_MODEL_PLUS, SYS_MODEL_GX4000, SYS_MODELS };
enum { SYS_MENU_QUIT = 0, SYS_MENU_POWER, SYS_MENU_CONTINUE, SYS_MENU_RESET, SYS_MENU_SAVE,
       SYS_MENU_SAVEGAME, SYS_MENU_FORGETGAME, SYS_MENU_SHOT };
int  Sys_Menu(struct SysMenu *m);
void Sys_MenuClose(void);           /* closes the menu screen Sys_Menu leaves open (call after the emulation screen is up) */
void Sys_MenuError(const char *text);
void Sys_ShowPicture(void *screen, const char *name);   /* amiga_iff.c: modal IFF viewer */

/* Workbench start (amiga_wb.c): fills st from the icon's ToolTypes and a
 * project icon; returns 1 when started from Workbench. */
struct SysStart { char prefs[SYS_PATH], media[SYS_PATH], remap[128]; int menu, hires, tall, nosound, skip, refresh, joykeys, pad, speed, video, model; };   /* skip -1, refresh 0, others -1: not given */
int Sys_ParseModel(const char *v);   /* "464" "664" "6128" "PLUS" "GX4000" (any case) -> SYS_MODEL_*, -1 if not one */
int Sys_ParseVideo(const char *v);   /* "AUTO" "PAL" "NTSC" or 0-2 -> 0-2, -1 if not one */
int Sys_ParseSpeed(const char *v);   /* "50" or 0 -> 0, "DISPLAY" or 1 -> 1, -1 if not one */
extern const char *Sys_ModelNames[SYS_MODELS], *Sys_VideoNames[3], *Sys_SpeedNames[2];   /* prefs words */
int  Sys_WBStart(int argc, char **argv, struct SysStart *st);

unsigned long Sys_Millis(void);

/* Performance counters (timer.device E-clock): time in c2p (conversion plus the
 * chip RAM writes), time waiting for flips/VBlank, lines converted. */
typedef struct { unsigned long c2p_us, wait_us, c2p_lines, c2p_frames, line_bytes,
                 wait_safe_us, wait_disp_us, wait_throttle_us, throttle_calls, late_frames,
                 pal_us, palette_loads, skipped_frames,
                 aud_underruns, aud_drops,
                 c2p_lines5, c2p_lines6, c2p_clear_planes, c2p_chip_longs,
                 vs_shifts, vs_recentres, vs_searches, vs_us,
                 audit_frames, audit_bad_frames, audit_bad_groups, audit_cop_checks, audit_cop_bad; } SysPerf;
extern int Sys_Audit;      /* AUDIT argument: read every converted buffer back from chip RAM and check it, and the copper's plane pointer */
extern int Sys_VShiftOn;   /* 1 when the display opened with the vertical-shift windows (NO_VSHIFT / NOVSHIFT: 0) */
extern int Sys_TallScreen;   /* the next Sys_OpenDisplay takes the full height (menu.tall) */
extern int Sys_NoVShift;   /* NOVSHIFT argument: don't use them (A/B tests) */
extern int Sys_VShiftOn_Last;   /* as Sys_VShiftOn for the last display opened (kept after closing, for the perf report) */
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
