/* Auto-start tapes (1.5): types the load command after Power ON / Reset and
 * presses a key at "Press PLAY then any key". Plain C, no OS calls; used by the
 * Amiga host and the headless host. */
#ifndef TAPEAUTO_H
#define TAPEAUTO_H
enum { TAPEAUTO_OFF = 0, TAPEAUTO_464, TAPEAUTO_DISC, TAPEAUTO_PLUS };
/* kind: TAPEAUTO_464 (RUN"), TAPEAUTO_DISC (664/6128: |TAPE then RUN"),
 * TAPEAUTO_PLUS (6128 Plus with the system cartridge: f1 for BASIC first) */
void TapeAuto_Start(int kind);
void TapeAuto_Stop(void);
int  TapeAuto_Active(void);
/* once a frame, at the frame end (where key scripts act): presses and releases
 * the CPC keys through setkey(key, down) */
void TapeAuto_Frame(void (*setkey)(int key, int down));
/* save states: 2 values (kind, frame) */
void TapeAuto_GetState(long *v);
void TapeAuto_SetState(const long *v);
#endif
