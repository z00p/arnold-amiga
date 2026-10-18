#ifndef KEYSCRIPT_H
#define KEYSCRIPT_H
#define KS_SNAP 1
#define KS_QUIT 2
int KeyScript_Load(const char *name);                        /* events loaded, -1 if no file */
int KeyScript_Frame(long frame, void (*setkey)(int key, int down));
int KeyScript_Active(void);
int KeyScript_SnapAt(long frame);                            /* a SNAP is due at this frame */
#endif
