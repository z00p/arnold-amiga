/* Key map logic: see keymaplogic.h. Plain C. */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "keymaplogic.h"

/* Presets: Controls > Key map. A preset just fills the remap list. */
const struct KeyPreset KeyMap_Presets[] = {
	{ "None",                                "" },
	{ "Pinball (Right Shift = M)",           "RSHIFT:M" },
	{ "Cursors to joystick, Space = fire",   "UP:JOYUP,DOWN:JOYDOWN,LEFT:JOYLEFT,RIGHT:JOYRIGHT,SPACE:FIRE1" },
	{ "Q A O P to joystick, Space = fire",   "Q:JOYUP,A:JOYDOWN,O:JOYLEFT,P:JOYRIGHT,SPACE:FIRE1" },
	{ "Right Alt / Right Amiga = fire 2",    "RALT:FIRE2,RAMIGA:FIRE2" },
	{ NULL, NULL }
};
int KeyMap_NPresets(void) { int n = 0; while (KeyMap_Presets[n].label) n++; return n; }
int KeyMap_PresetOf(const char *remap)
{
	int i;
	for (i = 0; KeyMap_Presets[i].label; i++) if (!strcmp(KeyMap_Presets[i].remap, remap)) return i;
	return -1;
}

static const struct { const char *name; unsigned char code; } amiga_keys[] = {
	{"LSHIFT", 0x60}, {"RSHIFT", 0x61}, {"CAPS", 0x62}, {"CTRL", 0x63}, {"LALT", 0x64}, {"RALT", 0x65},
	{"LAMIGA", 0x66}, {"RAMIGA", 0x67}, {"SPACE", 0x40}, {"BACKSPACE", 0x41}, {"TAB", 0x42},
	{"ENTER", 0x43}, {"RETURN", 0x44}, {"ESC", 0x45}, {"DEL", 0x46},
	{"UP", 0x4c}, {"DOWN", 0x4d}, {"RIGHT", 0x4e}, {"LEFT", 0x4f}, {NULL, 0} };
int KeyMap_AmigaNameCode(const char *s)
{
	int i;
	for (i = 0; amiga_keys[i].name; i++) if (!strcmp(amiga_keys[i].name, s)) return amiga_keys[i].code;
	return -1;
}
const char *KeyMap_AmigaCodeName(int code)
{
	int i;
	for (i = 0; amiga_keys[i].name; i++) if (amiga_keys[i].code == code) return amiga_keys[i].name;
	return NULL;
}

const char *const KeyMap_CpcNames[] = {
	"JOYUP", "JOYDOWN", "JOYLEFT", "JOYRIGHT", "FIRE1", "FIRE2",
	"SPACE", "RETURN", "ENTER", "SHIFT", "CTRL", "ESC", "TAB", "DEL", "CLR", "COPY", "CAPS",
	"LEFT", "RIGHT", "UP", "DOWN",
	"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
	"0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
	"F0", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "FDOT",
	"-", "^", "@", "[", "]", ";", ":", ",", ".", "/", "\\", NULL };
int KeyMap_NCpcNames(void) { int n = 0; while (KeyMap_CpcNames[n]) n++; return n; }

int KeyMap_Parse(const char *remap, struct KeyPair *p, int max)
{
	char buf[KM_STR], *t, *c;
	int n = 0;
	strncpy(buf, remap, KM_STR - 1); buf[KM_STR - 1] = 0;
	for (t = buf; *t; t++) *t = (char)toupper((unsigned char)*t);
	for (t = strtok(buf, ", "); t && n < max; t = strtok(NULL, ", ")) {
		c = strchr(t, ':'); if (!c) continue; *c++ = 0;
		if (!*t || !*c || strlen(t) >= sizeof p[0].a || strlen(c) >= sizeof p[0].c) continue;
		strcpy(p[n].a, t); strcpy(p[n].c, c); n++;
	}
	return n;
}

void KeyMap_Format(const struct KeyPair *p, int n, char *out, int outsz)
{
	int i, len = 0;
	out[0] = 0;
	for (i = 0; i < n; i++) {
		int l = (int)(strlen(p[i].a) + strlen(p[i].c) + 1 + (i ? 1 : 0));
		if (len + l >= outsz) break;
		len += sprintf(out + len, "%s%s:%s", i ? "," : "", p[i].a, p[i].c);
	}
}

int KeyMap_FitsString(const struct KeyPair *p, int n)
{
	char b[KM_STR * 2];
	int i, len = 0;
	for (i = 0; i < n; i++) len += (int)(strlen(p[i].a) + strlen(p[i].c) + 1 + (i ? 1 : 0));
	(void)b;
	return len < KM_STR;
}

int KeyMap_Set(struct KeyPair *p, int n, int max, const char *a, const char *c)
{
	int i;
	for (i = 0; i < n; i++) if (!strcmp(p[i].a, a)) { strcpy(p[i].c, c); return n; }
	if (n >= max) return -1;
	strcpy(p[n].a, a); strcpy(p[n].c, c);
	return n + 1;
}

void KeyMap_ShowName(const char *s, char *out)
{
	static const char *const nice[] = { "LSHIFT", "LShift", "RSHIFT", "RShift", "LALT", "LAlt", "RALT", "RAlt",
		"LAMIGA", "LAmiga", "RAMIGA", "RAmiga", "CTRL", "Ctrl", "CAPS", "Caps", "BACKSPACE", "BkSp", "RETURN", "Return", "ENTER", "Enter",
		"SPACE", "Space", "TAB", "Tab", "ESC", "Esc", "DEL", "Del", NULL };
	int i;
	for (i = 0; nice[i]; i += 2) if (!strcmp(nice[i], s)) { strcpy(out, nice[i + 1]); return; }
	strcpy(out, s);
}

void KeyMap_Summary(const char *remap, int maxlen, char *out)
{
	struct KeyPair p[KM_MAXPAIRS];
	int n = KeyMap_Parse(remap, p, KM_MAXPAIRS), i, len;
	char a[16], c[16];
	out[0] = 0;
	if (n == 0) return;
	len = sprintf(out, "Keys: ");
	for (i = 0; i < n; i++) {
		KeyMap_ShowName(p[i].a, a); KeyMap_ShowName(p[i].c, c);
		if (len + (int)strlen(a) + (int)strlen(c) + 2 > maxlen) { sprintf(out, "Keys: %d remapped", n); return; }
		len += sprintf(out + len, "%s%s=%s", i ? "," : "", a, c);
	}
}

void KeyMap_FitLine(char *out, int width, const char *full, const char *mid, const char *compact, const char *remap)
{
	char k[96], cnt[32];
	struct KeyPair p[KM_MAXPAIRS];
	int n = KeyMap_Parse(remap, p, KM_MAXPAIRS);
	if (n == 0) { strncpy(out, full, width); out[width] = 0; return; }
	KeyMap_Summary(remap, 90, k);
	sprintf(cnt, "Keys: %d remapped", n);
	if ((int)(strlen(full) + 2 + strlen(k)) <= width) sprintf(out, "%s  %s", full, k);
	else if ((int)(strlen(full) + 2 + strlen(cnt)) <= width) sprintf(out, "%s  %s", full, cnt);
	else if ((int)(strlen(mid) + 1 + strlen(k)) <= width) sprintf(out, "%s %s", mid, k);
	else if ((int)(strlen(mid) + 1 + strlen(cnt)) <= width) sprintf(out, "%s %s", mid, cnt);
	else if ((int)(strlen(compact) + 1 + strlen(k)) <= width) sprintf(out, "%s %s", compact, k);
	else sprintf(out, "%s %s", compact, cnt);
	out[width] = 0;
}
