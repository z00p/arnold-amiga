#ifndef KEYMAPLOGIC_H
#define KEYMAPLOGIC_H
/* Key map logic shared by the host, the menu and the editor (24 Oct 2026).
 * Plain C, no OS calls. A key map is the remap= string: "RSHIFT:M,UP:JOYUP". */
#define KM_STR 128          /* size of a remap string (the host's remap_str) */
#define KM_MAXPAIRS 16

/* THE preset table (keymaplogic.c): add a line there and the menu gets the entry */
struct KeyPreset { const char *label; const char *remap; };
extern const struct KeyPreset KeyMap_Presets[];
int KeyMap_NPresets(void);
int KeyMap_PresetOf(const char *remap);   /* index of the preset equal to this map, -1 for a custom map */

/* named Amiga keys (not letters, digits, F-keys) */
int KeyMap_AmigaNameCode(const char *upper);   /* name -> rawkey, -1 */
const char *KeyMap_AmigaCodeName(int code);    /* rawkey -> name, NULL */
/* CPC key names offered by the editor, in picker order */
extern const char *const KeyMap_CpcNames[];
int KeyMap_NCpcNames(void);

/* pairs: Amiga name / CPC name text */
struct KeyPair { char a[12], c[10]; };
int  KeyMap_Parse(const char *remap, struct KeyPair *p, int max);   /* count; names upper-cased */
void KeyMap_Format(const struct KeyPair *p, int n, char *out, int outsz);
int  KeyMap_Set(struct KeyPair *p, int n, int max, const char *a, const char *c);   /* add or replace by Amiga key; new count, -1 if full */
int  KeyMap_FitsString(const struct KeyPair *p, int n);   /* the formatted map fits KM_STR */

/* display */
void KeyMap_ShowName(const char *upper, char *out);   /* RSHIFT -> RShift */
/* "Keys: RShift=M", or "Keys: 3 remapped" when it doesn't fit; "" for no map */
void KeyMap_Summary(const char *remap, int maxlen, char *out);
/* the settings line: full, medium (Ref/Joy) or compact (Snd/Scr/Ref/Joy) text plus the keys part, in at most width chars */
void KeyMap_FitLine(char *out, int width, const char *full, const char *mid, const char *compact, const char *remap);
#endif
