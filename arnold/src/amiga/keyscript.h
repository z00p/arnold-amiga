#ifndef KEYSCRIPT_H
#define KEYSCRIPT_H
#define KS_SNAP 1
#define KS_QUIT 2
#define KS_AMIGA 0x4000   /* script key = KS_AMIGA | Amiga rawkey */
int KeyScript_KeyId(const char *name);   /* CPC key name -> CPC_KEY_*, -1 unknown */
extern int (*KeyScript_AmigaKey)(const char *name);
int KeyScript_Load(const char *name);                        /* events loaded, -1 if no file */
int KeyScript_Frame(long frame, void (*setkey)(int key, int down));
int KeyScript_Active(void);
int KeyScript_SnapAt(long frame);                            /* a SNAP is due at this frame */
void KeyScript_Seek(long frame);                             /* go on from this frame (save states) */
#endif
