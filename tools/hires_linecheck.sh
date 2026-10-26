#!/bin/bash
# hires_linecheck.sh : is the core's changed-line tracking (Render_LineChangedHook) right? (item 3 of 1.2)
# Builds the headless host with a test patch: the chunky buffer is 768x544 (hires), HIRES=1 selects
# RENDERING_ACCURACY_HIGH as the Amiga host's HIRES option does, and a line hook is installed. After each
# frame every row that differs from the previous frame is compared with the rows the hook named:
#   STALE   = row changed, hook not called for it (an Amiga screen would keep the old picture)
#   spur    = hook called, row did not change (harmless extra work)
# usage: bash tools/hires_linecheck.sh   (WSL; A=<worktree> D=<private build dir>)
export PATH=/usr/local/bin:/usr/bin:/bin
A=${A:-$(cd "$(dirname "$0")/.." && pwd)/build/gamedb}
D=${D:-/opt/src/gd-linecheck}
rm -rf $D; mkdir -p $D
rsync -a --exclude 'obj-*' --exclude 'Arnold*' $A/arnold/src/ $D/src/
cd $D/src
cat > $D/patch.py <<'PY'
s = open('headless/host_headless.c').read()
s = s.replace('#define W 384', '#define W 768').replace('#define H 272', '#define H 544')
s = s.replace('void Host_SwapGraphicsBuffers(void) { frames++;', """#define HIRESON (getenv("HIRES") && *getenv("HIRES"))
extern void (*Render_LineChangedHook)(int);
static unsigned char lc_mark[H], lc_prev[W*H]; static long lc_frames, lc_changed, lc_stale, lc_spur, lc_first;
static void lc_hook(int y) { if (y >= 0 && y < H) lc_mark[y] = 1; }
static void lc_check(void)
{
	int y;
	lc_frames++;
	for (y = 0; y < H; y++) {
		int ch = memcmp(lc_prev + y*W, chunky + y*W, W) != 0;
		lc_changed += ch;
		if (ch && !lc_mark[y]) { lc_stale++; if (!lc_first) lc_first = lc_frames * 1000 + y; }
		if (!ch && lc_mark[y]) lc_spur++;
		lc_mark[y] = 0;
	}
	memcpy(lc_prev, chunky, W*H);
}
void Host_SwapGraphicsBuffers(void) { frames++; lc_check();""")
s = s.replace('\tRender_SetDisplayWindowed();', '\tif (HIRESON) Render_SetRenderingAccuracyForWindowedMode(RENDERING_ACCURACY_HIGH);\n\tRender_LineChangedHook = lc_hook;\n\tRender_SetDisplayWindowed();')
s = s.replace('\tdump();\n\tsave_disk(0, "DISKOUT");', '\tprintf("LINECHECK %s: frames %ld changed rows %ld STALE %ld spur %ld (first stale: frame %ld row %ld)\\n", HIRESON ? "hires" : "lores", lc_frames, lc_changed, lc_stale, lc_spur, lc_first / 1000, lc_first % 1000);\n\tdump();\n\tsave_disk(0, "DISKOUT");')
open('headless/host_headless.c', 'w').write(s)
PY
python3 $D/patch.py
SRCS="$(tr -d '\r' < objects.mak | sed -n '/^CPC_O=/,/^$/p' | tr -d '\\' | sed 's/CPC_O=//;s/\.o/.c/g') headless/host_headless.c amiga/keyscript.c"
gcc -O2 -std=gnu89 -fcommon -I. -DUNIX -DCPC_LSB_FIRST -DCPC_NODEBUGGER -w -o $D/hd $SRCS -lm 2>&1 | head
KEYS=$A/tests/keys/mode2_scroll.keys
SYS=$A/arnold/roms/cpcplus/system.cpr
for h in "" 1; do
  echo "--- BASIC mode 2 scrolling, $([ -n "$h" ] && echo hires || echo lores)"
  HIRES=$h $D/hd 1000 /tmp/gd_lc.ppm $SYS 8 $KEYS | grep LINECHECK
done
for c in "01 Sonic GX" "12 Bomb Jack GX"; do
  echo "--- $c (hires)"
  HIRES=1 $D/hd 1500 /tmp/gd_lcs.ppm "$A/carts/homebrew/$c.cpr" 8 $A/tests/keys/generic.keys | grep LINECHECK
done
