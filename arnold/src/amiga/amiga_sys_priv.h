/* Arnold on AmigaOS: what amiga_sys.c shares with the files split off it
 * (split16, 30 Oct 2026): former statics one other file needs. Only for
 * amiga_sys.c and those files; host_amiga.c uses amiga_sys.h. */
#ifndef AMIGA_SYS_PRIV_H
#define AMIGA_SYS_PRIV_H

/* amiga_audio.c: the block queue, for the heartbeat line (Sys_Heartbeat) */
extern volatile int Sys_AudioCount;
extern volatile unsigned long Sys_AudioUnderruns, Sys_AudioDrops;

#endif
