/* Arnold on AmigaOS: Paula sound (audio.device channels 0 and 1, the queue of
 * blocks and the interrupt that feeds them), and the sound's DC filter for save
 * states and fast-forward. Split off amiga_sys.c (split16, 30 Oct 2026). */
#include <exec/types.h>
#include <exec/memory.h>
#include <hardware/custom.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "amiga_sys.h"
#include "amiga_sys_priv.h"

extern struct Custom custom;

/* ---- sound ---- */
/* Paula, channels 0 (left) and 1 (right), 8-bit signed (14 Oct 2026 rewrite).
 * The emulation makes one block a CPC frame (444 samples). A frame of the PAL
 * display is 313 x 227 = 71051 colour clocks; at period 160 a block lasts
 * 71040, so one block per VBlank keeps Paula fed at the display's pace. Blocks
 * go into a small queue; a VBlank interrupt server hands the oldest one to
 * Paula (it is latched when the playing block ends). No block ready (the
 * emulation is behind): Paula repeats the last one. Queue full (it caught up in
 * a burst): the oldest waiting block is dropped. Both are counted (Sys_Perf). */
#include <devices/audio.h>
#include <hardware/dmabits.h>
#include <hardware/intbits.h>
#include <exec/interrupts.h>
#include "audio_conv.h"
#define ABUFS 4                       /* playing + handed over + up to 2 queued */
static struct MsgPort *audport;
static struct IOAudio *audio;
static int aud_open, aud_n;
static UBYTE *aud_buf[2][ABUFS];      /* [channel][slot] in chip RAM */
static UBYTE aud_map[] = { 3 };       /* channels 0 and 1 */
static volatile int aud_rd, aud_wr, aud_run;
static volatile unsigned long aud_irqs;
volatile int Sys_AudioCount;                      /* amiga_sys_priv.h: Sys_Heartbeat logs the queue */
volatile unsigned long Sys_AudioUnderruns, Sys_AudioDrops;
static struct Interrupt aud_vbint;
static int aud_vbint_added;
/* 19 Oct 2026: on a display that isn't 50 Hz the VBlank can't pace Paula, so
 * the blocks are handed over by channel 0's own interrupt instead: Paula
 * raises it when it latches a block (the start of its playback), and the
 * pointer written then is latched when that block ends. The rate is then
 * Paula's own: samples x period colour clocks a block. aud_mode 0 = VBlank
 * server (PAL, as tuned), 1 = audio interrupt. */
static int aud_mode, aud_first;
static struct Interrupt aud_int, *aud_oldint;

/* HDMI sound (Nov 2026, branch hdmiahi): Sys_AudioAhi set (menu, RTG screen only) sends
 * the sound to AHI, mode 001f0012 = emu68hdmi.audio 16 bit low latency. SetEnv ArnoldAHI
 * <mode id in hex> tries another mode, on any screen. No AHI or no driver: Paula. */
int Sys_AudioAhi;
#if !defined(NO_AHI) && !defined(NO_SNDLUT_FIX) && !defined(NO_SNDUNIPOLAR)
#define USE_AHI
static int  Ahi_Open(int period, int samples);
static void Ahi_Close(void);
static void Ahi_Queue(const unsigned short *s16, int samples);
static void Ahi_Pause(void);
static int ahi_on;
#endif

static void aud_feed(void)
{
	if (aud_run) {
		if (Sys_AudioCount > 0) {
			custom.aud[0].ac_ptr = (UWORD *)aud_buf[0][aud_rd];
			custom.aud[1].ac_ptr = (UWORD *)aud_buf[1][aud_rd];
			aud_rd = (aud_rd + 1) % ABUFS;
			Sys_AudioCount--;
		} else if (aud_first)
			aud_first = 0;   /* the start: nothing queued behind the first block yet */
		else
			Sys_AudioUnderruns++;
	}
}

static LONG aud_vblank(register APTR data __asm("a1"))
{
	aud_feed();
	return 0;
}

/* exec interrupt handler (SetIntVector): must clear its request itself */
static void aud_handler(register APTR data __asm("a1"))
{
	custom.intreq = INTF_AUD0;
	aud_irqs++;
	aud_feed();
}

unsigned long Sys_AudioIrqs(void) { return aud_irqs; }
int Sys_AudioAhiOn(void)
{
#ifdef USE_AHI
	return ahi_on;
#else
	return 0;
#endif
}

int Sys_AudioOpen(int period, int samples, int audint)
{
	int c, b;
	if (aud_open) return 1;
#ifdef USE_AHI
	if (Ahi_Open(period, samples)) return 1;
#endif
	aud_mode = audint;
	if (samples & 1) samples--;
	aud_n = samples;
	audport = CreateMsgPort();
	if (!audport) return 0;
	audio = (struct IOAudio *)CreateIORequest(audport, sizeof(struct IOAudio));
	if (!audio) { DeleteMsgPort(audport); audport = NULL; return 0; }
	audio->ioa_Request.io_Message.mn_Node.ln_Pri = 127;   /* don't let others steal them */
	audio->ioa_Request.io_Command = ADCMD_ALLOCATE;
	audio->ioa_Request.io_Flags = ADIOF_NOWAIT;
	audio->ioa_AllocKey = 0;
	audio->ioa_Data = aud_map;
	audio->ioa_Length = sizeof(aud_map);
	if (OpenDevice("audio.device", 0, (struct IORequest *)audio, 0) != 0) {
		Sys_Log("audio: audio.device channels 0+1 not free");
		DeleteIORequest((struct IORequest *)audio); audio = NULL;
		DeleteMsgPort(audport); audport = NULL;
		return 0;
	}
	aud_open = 1;
	for (c = 0; c < 2; c++) for (b = 0; b < ABUFS; b++) {
		aud_buf[c][b] = AllocMem(aud_n, MEMF_CHIP | MEMF_CLEAR);
		if (!aud_buf[c][b]) { Sys_Log("audio: no chip RAM for buffers"); Sys_AudioClose(); return 0; }
	}
	custom.aud[0].ac_per = period; custom.aud[1].ac_per = period;
	custom.aud[0].ac_vol = 64;     custom.aud[1].ac_vol = 64;
	custom.aud[0].ac_len = aud_n >> 1; custom.aud[1].ac_len = aud_n >> 1;
	aud_rd = aud_wr = Sys_AudioCount = 0; aud_run = 0;
	if (aud_mode) {
		aud_int.is_Node.ln_Type = NT_INTERRUPT;
		aud_int.is_Node.ln_Pri = 0;
		aud_int.is_Node.ln_Name = (char *)"Arnold audio";
		aud_int.is_Data = NULL;
		aud_int.is_Code = (VOID (*)())aud_handler;
		custom.intena = INTF_AUD0;
		custom.intreq = INTF_AUD0;
		aud_oldint = SetIntVector(INTB_AUD0, &aud_int);
		custom.intena = INTF_SETCLR | INTF_AUD0;
	} else {
		aud_vbint.is_Node.ln_Type = NT_INTERRUPT;
		aud_vbint.is_Node.ln_Pri = 0;
		aud_vbint.is_Node.ln_Name = (char *)"Arnold audio";
		aud_vbint.is_Data = NULL;
		aud_vbint.is_Code = (VOID (*)())aud_vblank;
		AddIntServer(INTB_VERTB, &aud_vbint);
		aud_vbint_added = 1;
	}
	Sys_Log("audio: open, period %d, %d samples a block, fed by %s, chip clock %lu Hz", period, samples,
		aud_mode ? "the audio interrupt" : "the VBlank", Sys_ChipClock());
	return 1;
}

void Sys_AudioClose(void)
{
	int c, b;
#ifdef USE_AHI
	if (ahi_on) { Ahi_Close(); return; }
#endif
	if (!aud_open) return;
	aud_run = 0;
	if (aud_vbint_added) { RemIntServer(INTB_VERTB, &aud_vbint); aud_vbint_added = 0; }
	custom.dmacon = DMAF_AUD0 | DMAF_AUD1;   /* clear */
	if (aud_mode) {
		custom.intena = INTF_AUD0;
		SetIntVector(INTB_AUD0, aud_oldint);
		custom.intreq = INTF_AUD0;
		aud_oldint = NULL; aud_mode = 0;
	}
	custom.aud[0].ac_vol = 0; custom.aud[1].ac_vol = 0;
	for (c = 0; c < 2; c++) for (b = 0; b < ABUFS; b++)
		if (aud_buf[c][b]) { FreeMem(aud_buf[c][b], aud_n); aud_buf[c][b] = NULL; }
	if (audio) { CloseDevice((struct IORequest *)audio); DeleteIORequest((struct IORequest *)audio); audio = NULL; }
	if (audport) { DeleteMsgPort(audport); audport = NULL; }
	aud_open = 0;
}

void Sys_AudioQueue(const unsigned char *s16, int samples)
{
	int slot;
#ifdef USE_AHI
	if (ahi_on) { Ahi_Queue((const unsigned short *)s16, samples); return; }
#endif
	if (!aud_open) return;
	if (samples > aud_n) samples = aud_n;
	Disable();
	if (Sys_AudioCount >= ABUFS - 2) {             /* full: drop the oldest waiting block */
		aud_rd = (aud_rd + 1) % ABUFS;
		Sys_AudioCount--;
		Sys_AudioDrops++;
	}
	slot = aud_wr;
	Enable();
	/* Slots in use: the block playing, the one handed over at the last VBlank
	 * (latched when the playing one ends) and the queue; with the queue kept
	 * below ABUFS - 2 the slot at aud_wr is none of them. */
	Audio_SplitTo8((const unsigned short *)s16, (signed char *)aud_buf[0][slot], (signed char *)aud_buf[1][slot], samples);
	if (samples < aud_n) {                    /* short block: hold the last level */
		memset(aud_buf[0][slot] + samples, samples ? aud_buf[0][slot][samples - 1] : 0, aud_n - samples);
		memset(aud_buf[1][slot] + samples, samples ? aud_buf[1][slot][samples - 1] : 0, aud_n - samples);
	}
#ifdef SOUNDQ_DUMP
	/* soundq verification builds only: every block as Paula gets it (left then
	   right, 8-bit signed) to PROGDIR:audio.raw, to compare with the headless WAV */
	{ static BPTR fh; static int tried;
	  if (!tried) { tried = 1; fh = Open((STRPTR)"PROGDIR:audio.raw", MODE_NEWFILE); }
	  if (fh) { Write(fh, aud_buf[0][slot], samples); Write(fh, aud_buf[1][slot], samples); Flush(fh); }
	  { static BPTR f16; static int tried16;   /* and the core's 16-bit block (big-endian) */
	    if (!tried16) { tried16 = 1; f16 = Open((STRPTR)"PROGDIR:audio16.raw", MODE_NEWFILE); }
	    if (f16) { Write(f16, (APTR)s16, samples * 4); Flush(f16); } } }
#endif
	Disable();
	aud_wr = (aud_wr + 1) % ABUFS;
	Sys_AudioCount++;
	if (!aud_run) {
		/* start: this block plays at once, the server feeds the rest */
		custom.aud[0].ac_ptr = (UWORD *)aud_buf[0][slot];
		custom.aud[1].ac_ptr = (UWORD *)aud_buf[1][slot];
		aud_rd = aud_wr; Sys_AudioCount = 0;
		aud_first = 1;
		if (aud_mode) custom.intreq = INTF_AUD0;
		custom.dmacon = DMAF_SETCLR | DMAF_MASTER | DMAF_AUD0 | DMAF_AUD1;
		if (aud_mode) custom.intena = INTF_SETCLR | INTF_AUD0;   /* off while paused, see Sys_AudioPause */
		aud_run = 1;
	}
	Enable();
	Sys_Perf.aud_underruns = Sys_AudioUnderruns;
	Sys_Perf.aud_drops = Sys_AudioDrops;
}

void Sys_AudioPause(void)
{
#ifdef USE_AHI
	if (ahi_on) { Ahi_Pause(); return; }
#endif
	if (!aud_open) return;
	Disable();
	aud_run = 0;
	aud_rd = aud_wr = Sys_AudioCount = 0;
	/* 19 Oct 2026 (NTSC menu shear on John's A1200): with the audio DMA off,
	 * an enabled AUD0 interrupt doesn't stop. Paula drops to manual mode and
	 * asks for a new word every sample period once the handler clears the
	 * request: about 22000 level-4 interrupts a second for as long as the
	 * menu was up (70000 counted in one WinUAE menu visit). They hold off the
	 * level-3 VBlank work of graphics and Intuition. So the interrupt is
	 * switched off here, and Sys_AudioQueue switches it on again with the DMA. */
	if (aud_mode) custom.intena = INTF_AUD0;
	custom.dmacon = DMAF_AUD0 | DMAF_AUD1;
	if (aud_mode) custom.intreq = INTF_AUD0;
	Enable();
}

/* Save states (1.4, 28 Oct 2026; host_amiga.c): the DC filter of the sound
 * (audio_conv.h, above) is part of a state so the sound after a load is the same
 * to the sample. */
void Sys_AudioDcState(long *v, int set)
{
#if !defined(NO_SNDLUT_FIX) && !defined(NO_SNDUNIPOLAR)
	if (set) { Audio_DcX[0] = v[0]; Audio_DcX[1] = v[1]; Audio_DcY[0] = v[2]; Audio_DcY[1] = v[3]; Audio_DcOn = (int)v[4]; }
	else { v[0] = Audio_DcX[0]; v[1] = Audio_DcX[1]; v[2] = Audio_DcY[0]; v[3] = Audio_DcY[1]; v[4] = Audio_DcOn; }
#else
	if (!set) v[0] = v[1] = v[2] = v[3] = v[4] = 0;
#endif
}

/* Fast-forward (1.5, 28 Oct 2026; host_amiga.c): a block that is not played still
 * goes through the DC filter, so the sound after fast-forward is the same to the
 * sample as in a run without it. */
void Sys_AudioSkip(const unsigned char *s16, int samples)
{
	static signed char l[1024], r[1024];
	if (samples > 1024) samples = 1024;
	Audio_SplitTo8((const unsigned short *)s16, l, r, samples);
}

#ifdef USE_AHI
/* AHI back end: the frame's block in 16 bits, after the same DC filter as Paula's, in a
 * ring of slots in fast RAM played as one dynamic sound. SoundFunc runs when a slot
 * starts and queues the next, as Paula latches its pointer: no block ready, the slot
 * repeats. AHI's clock isn't the frame's, so the rate moves 0.2% to hold the queue. */
#include <devices/ahi.h>
#include <dos/var.h>
#include <proto/ahi.h>
#define AHI_HDMI_MODE 0x001f0012
#define AHI_SLOTS 8
#define AHI_START 2                   /* blocks queued before the sound starts */
#define AHI_QMAX  4
struct Library *AHIBase;
static struct MsgPort *ahi_port;
static struct AHIRequest *ahi_io;
static struct AHIAudioCtrl *ahi_ctrl;
static struct Hook ahi_hook;
static WORD *ahi_buf;
static int ahi_n, ahi_dev;
static volatile int ahi_rd, ahi_wr, ahi_run;
static ULONG ahi_rate;

static ULONG ahi_sound(register struct Hook *h __asm("a0"), register struct AHIAudioCtrl *c __asm("a2"),
	register struct AHISoundMessage *m __asm("a1"))
{
	if (ahi_run) {
		if (Sys_AudioCount > 0) {
			AHI_SetSound(0, 0, ahi_rd * ahi_n, ahi_n, c, AHISF_NONE);
			ahi_rd = (ahi_rd + 1) % AHI_SLOTS;
			Sys_AudioCount--;
		} else
			Sys_AudioUnderruns++;
		AHI_SetFreq(0, Sys_AudioCount < 1 ? ahi_rate - ahi_rate / 500 :
			Sys_AudioCount > 2 ? ahi_rate + ahi_rate / 500 : ahi_rate, c, AHISF_NONE);
	}
	return 0;
}

static int Ahi_Open(int period, int samples)
{
	static struct AHISampleInfo si;
	char v[16], *p;
	ULONG mode = 0, maxplay = 0;
	if (GetVar((STRPTR)"ArnoldAHI", (STRPTR)v, sizeof(v), GVF_GLOBAL_ONLY) > 0) {
		for (p = v; *p >= '0'; p++) mode = (mode << 4) | (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
	} else if (Sys_AudioAhi)
		mode = AHI_HDMI_MODE;
	else
		return 0;
	if (samples & 1) samples--;
	ahi_n = samples;
	ahi_rate = Sys_ChipClock() / period;
	ahi_buf = AllocMem(AHI_SLOTS * ahi_n * 4, MEMF_PUBLIC | MEMF_CLEAR);
	ahi_port = CreateMsgPort();
	if (ahi_port) ahi_io = (struct AHIRequest *)CreateIORequest(ahi_port, sizeof(struct AHIRequest));
	if (!ahi_buf || !ahi_io) goto fail;
	ahi_io->ahir_Version = 4;
	if (OpenDevice((STRPTR)AHINAME, AHI_NO_UNIT, (struct IORequest *)ahi_io, 0)) goto fail;
	ahi_dev = 1;
	AHIBase = (struct Library *)ahi_io->ahir_Std.io_Device;
	ahi_hook.h_Entry = (ULONG (*)())ahi_sound;
	ahi_ctrl = AHI_AllocAudio(AHIA_AudioID, mode, AHIA_MixFreq, 48000, AHIA_Channels, 1, AHIA_Sounds, 1,
		AHIA_SoundFunc, (ULONG)&ahi_hook, TAG_DONE);
	if (!ahi_ctrl) goto fail;
	AHI_GetAudioAttrs(AHI_INVALID_ID, ahi_ctrl, AHIDB_MaxPlaySamples, (ULONG)&maxplay, TAG_DONE);
	si.ahisi_Type = AHIST_S16S;
	si.ahisi_Address = ahi_buf;
	si.ahisi_Length = AHI_SLOTS * ahi_n;
	if (AHI_LoadSound(0, AHIST_DYNAMICSAMPLE, &si, ahi_ctrl)) goto fail;
	if (AHI_ControlAudio(ahi_ctrl, AHIC_Play, TRUE, TAG_DONE)) goto fail;
	AHI_SetVol(0, 0x10000, 0x8000, ahi_ctrl, AHISF_IMM);
	AHI_SetFreq(0, ahi_rate, ahi_ctrl, AHISF_IMM);
	ahi_rd = ahi_wr = Sys_AudioCount = 0; ahi_run = 0;
	ahi_on = 1; aud_open = 1;
	Sys_Log("audio: AHI mode %08lx, %lu Hz, %d samples a block, max play %lu", mode, ahi_rate, ahi_n, maxplay);
	return 1;
fail:
	Sys_Log("audio: AHI mode %08lx not available, using Paula", mode);
	Ahi_Close();
	return 0;
}

static void Ahi_Close(void)
{
	ahi_run = 0;
	if (ahi_ctrl) {
		AHI_ControlAudio(ahi_ctrl, AHIC_Play, FALSE, TAG_DONE);
		AHI_FreeAudio(ahi_ctrl);
		ahi_ctrl = NULL;
	}
	if (ahi_dev) { CloseDevice((struct IORequest *)ahi_io); ahi_dev = 0; }
	if (ahi_io) { DeleteIORequest((struct IORequest *)ahi_io); ahi_io = NULL; }
	if (ahi_port) { DeleteMsgPort(ahi_port); ahi_port = NULL; }
	if (ahi_buf) { FreeMem(ahi_buf, AHI_SLOTS * ahi_n * 4); ahi_buf = NULL; }
	ahi_on = 0; aud_open = 0;
}

/* Audio_DcBlock with the sound kept in 16 bits */
static WORD ahi_dc(int side, long x)
{
	long y, o;
	if (!Audio_DcOn) { Audio_DcOn = 1; Audio_DcX[0] = Audio_DcX[1] = x; Audio_DcY[0] = Audio_DcY[1] = 0; }
	y = Audio_DcY[side] + ((x - Audio_DcX[side]) << 8) - (Audio_DcY[side] >> 8);
	Audio_DcX[side] = x; Audio_DcY[side] = y;
	o = (y + 128) >> 8;
	if (o > 32767) o = 32767; else if (o < -32768) o = -32768;
	return (WORD)o;
}

static void Ahi_Queue(const unsigned short *s16, int samples)
{
	int slot, i, first = -1;
	WORD *d;
	if (samples > ahi_n) samples = ahi_n;
	Disable();
	if (Sys_AudioCount >= AHI_QMAX) {             /* full: drop the oldest waiting block */
		ahi_rd = (ahi_rd + 1) % AHI_SLOTS;
		Sys_AudioCount--;
		Sys_AudioDrops++;
	}
	slot = ahi_wr;
	Enable();
	d = ahi_buf + slot * ahi_n * 2;
	for (i = 0; i < samples; i++) {
		d[2 * i] = ahi_dc(0, (long)s16[2 * i]);
		d[2 * i + 1] = ahi_dc(1, (long)s16[2 * i + 1]);
	}
	for (; i < ahi_n; i++) {                      /* short block: hold the last level */
		d[2 * i] = i ? d[2 * i - 2] : 0;
		d[2 * i + 1] = i ? d[2 * i - 1] : 0;
	}
	Disable();
	ahi_wr = (ahi_wr + 1) % AHI_SLOTS;
	Sys_AudioCount++;
	if (!ahi_run && Sys_AudioCount >= AHI_START) {
		first = ahi_rd;
		ahi_rd = (ahi_rd + 1) % AHI_SLOTS;
		Sys_AudioCount--;
		ahi_run = 1;
	}
	Enable();
	if (first >= 0) AHI_SetSound(0, 0, first * ahi_n, ahi_n, ahi_ctrl, AHISF_IMM);
	Sys_Perf.aud_underruns = Sys_AudioUnderruns;
	Sys_Perf.aud_drops = Sys_AudioDrops;
}

static void Ahi_Pause(void)
{
	ahi_run = 0;
	AHI_SetSound(0, AHI_NOSOUND, 0, 0, ahi_ctrl, AHISF_IMM);
	Disable();
	ahi_rd = ahi_wr = Sys_AudioCount = 0;
	Enable();
}
#endif
