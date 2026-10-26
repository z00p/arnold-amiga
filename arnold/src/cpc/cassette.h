#ifndef __CASSETTE_HEADER_INCLUDED__
#define __CASSETTE_HEADER_INCLUDED__


typedef enum
{
	CASSETTE_TYPE_SAMPLE = 0,
	CASSETTE_TYPE_TAPE_IMAGE,
	CASSETTE_TYPE_NONE
} CPC_CASSETTE_TYPE_ID;

void	CPC_SetCassetteType(int);

unsigned long Cassette_Read(unsigned long NopsPassed);
void	Cassette_Write(unsigned long NopsPassed, unsigned long State);
void	Cassette_Init(void);
void	Cassette_Finish(void);

unsigned long CPC_GetCassetteType(void);


void	Tape_Remove(void);
/* Amiga port: the cassette motor (PPI port C bit 4) is on */
int	CPC_GetTapeMotor(void);

/* Amiga port (tape sound, 25 Oct 2026): the tape signal as sound, like CPCEC's
 * tape_loud. The host sets TapeAudio_On; while it is set, Cassette_Read notes
 * each change of the level it hands the Z80, and the host calls TapeAudio_Mix
 * once a frame on the finished block of 16-bit stereo samples (audio_conv.h
 * units: 0..32383). Off, nothing is recorded or mixed. */
extern int TapeAudio_On;
extern unsigned long TapeAudio_Changes;   /* level changes noted so far (for the log) */
void	TapeAudio_Note(int bit);
void	TapeAudio_Reset(void);   /* forget the clock and the level (the option was switched) */
void	TapeAudio_Mix(unsigned short *s16, int samples);


#endif

