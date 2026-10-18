#ifndef SCREENSHOT_H
#define SCREENSHOT_H
int Screenshot_SaveILBM(const char *name, const unsigned char *chunky, int w, int h, int pitch,
                        const unsigned char pal[256][3], int hires);
int Screenshot_SavePNG(const char *name, const unsigned char *chunky, int w, int h, int pitch,
                       const unsigned char pal[256][3]);
#endif
