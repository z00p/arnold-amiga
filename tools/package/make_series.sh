#!/bin/bash
# Build the upstream patch series on branch upstream-patches (worktree build/upstream-wt).
# Setup: git worktree add build/upstream-wt -b upstream-patches e5dce08
# Then: git format-patch -o <dir> e5dce08..upstream-patches. hscroll.py / prifix.py
# replay the two Plus fixes so they become separate patches.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
PKG="$R/tools/package"
WT=$R/build/upstream-wt
cd $R
TRAILER="Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"

pre()  { git archive "$1" "${@:2}" | tar -x -C $WT; }
post() { git archive "$1" "${@:2}" | tar -x -C $WT --strip-components=1; }
commit() {  # subject, body
  git -C $WT add -A src
  if git -C $WT diff --cached --quiet; then echo "(nothing for: $1)"; return; fi
  printf '%s\n\n%s\n\n%s\n' "$1" "$2" "$TRAILER" | git -C $WT -c user.name="John Stringer" -c user.email="jcec_1@yahoo.co.uk" commit -q -F -
  echo "ok: $1 ($(git -C $WT diff --shortstat HEAD^ HEAD))"
}

pre 7db09e7 src/cpc/cpcdefs.h
commit "Build the core on big-endian hosts (m68k)" \
"cpcdefs.h defined CPC_LSB_FIRST unconditionally, so a big-endian build had
byte-swapped words throughout. Leave it to the build system (-DCPC_LSB_FIRST
on little-endian hosts, which src/Makefile already passes) so the core builds
and runs on 68k AmigaOS."

pre 8ab24dd src/cpc/render5.c
commit "render: optional hook telling the host which lines changed" \
"Render_LineChangedHook (NULL by default: unchanged behaviour). When a host
sets it, Render_DumpScreen4 copies only the lines that differ from the last
frame and calls the hook for each, so a slow host (Amiga chunky-to-planar)
converts only those lines."

pre b58aa60 src/cpc
commit "Speed-ups A-D for slow hosts (pixel-exact)" \
"A: skip cells outside the visible area.  B: cache the pixel longs a cell
draws.  C: per-column sprite mask so columns with no sprite skip the sprite
merge.  D: inline paletted cell path in Graphics_Update.
Every frame of the test set is identical to the unmodified renderer (headless
checksums). NO_A / NO_B / NO_C / NO_D restore the original code."

pre c9cc2f9 src/cpc
commit "Speed-up E: inline Z80 data memory access, run-based CRTC stepping" \
"The CRTC is stepped in runs up to its next event instead of one character at
a time, and Z80 data reads/writes through the memory map are inlined.
Pixel-exact on the test set; NO_E restores the per-step loop."

pre 07ecc75 src/cpc
commit "Speed-up F: HALT fast-forward" \
"While the Z80 is halted, its cycles are batched up to the next CRTC event that
can raise an interrupt instead of being stepped one NOP at a time.
Pixel-exact on the test set; NO_F restores per-step."

pre 023f237 src/cpc/audioevent.c
commit "Fix the 16-bit audio output conversion" \
"AudioEvent_ConvertToOutputFormat_16BitStereo/Mono read the event buffer as
16-bit words, but it holds 4 bytes a sample (left, right, 2 unused; see
AudioEvent_UpdateCycle). Two samples were consumed per output sample: the
first half of each block played at half speed and the rest was silence (on a
big-endian host also the wrong byte). Read it a byte at a time."

pre 023f237 src/cpc
commit "Frame skip: decide one frame ahead so drawn frames start cleanly" \
"The core's frame skip turns drawing back on at the frame boundary, which falls
mid-line: the draw position along that line and the Plus soft-scroll shift
register are stale, and the rest of the line lands in the wrong place (one
wrong row per drawn frame; FrameSkip has the same flaw). CPC_SkipNextFrameHook
lets a host decide at each frame end whether the frame after next is drawn;
a skipped frame followed by a drawn one turns drawing on RENDER_LEAD cycles
before its end, and only frames meant to be drawn are passed to the host.
NULL hook: unchanged behaviour."

python - "$WT/src/cpc" "$PKG" <<'PY'
import sys, os
d = sys.argv[1]
src = open(os.path.join(sys.argv[2], 'hscroll.py')).read()
exec(compile(src.replace('os.chdir(REPO_CPC)', 'os.chdir(%r)' % d), 'hscroll', 'exec'))
PY
commit "Plus: horizontal soft scroll in modes 0 and 1" \
"The Plus soft scroll delays the pixel stream by whole pixels of the current
mode. The paletted renderer shifted the two graphics bytes as plain bits,
which is right only in mode 2: in modes 0 and 1 a pixel's bits are
interleaved across the byte, so the shift mixed bits of neighbouring pixels
(outlines right, colours scrambled: Sonic GX throughout, Ghosts'n Goblins at
times). Render_ScrollGraphicsData does the per-pixel shift the true-colour
renderer already uses; the pixel count is also kept in step with mode changes.
Checked against CPCEC frame by frame. NO_HSCROLL_FIX restores the old shift."

python - "$WT/src/cpc" "$PKG" <<'PY'
import sys, os
d = sys.argv[1]
src = open(os.path.join(sys.argv[2], 'prifix.py')).read()
exec(compile(src.replace('os.chdir(REPO_CPC)', 'os.chdir(%r)' % d), 'prifix', 'exec'))
PY
commit "Plus: raise the programmable raster interrupt at HSYNC start" \
"The PRI was compared at HSYNC end, ~R3 characters late. Sonic GX chains PRI
interrupts (each handler sets PRI to the next line) and missed its next line,
so its split screens and palette changes stopped. Raise it when HSYNC starts
on the matching line (as CPCEC does), and raise it at once when PRI is written
with the current line while HSYNC is active. NO_PRI_FIX restores the old
timing."

post f195f96 arnold/src/cpc
commit "Diagnostics for hardware tracing (off by default)" \
"TRACE_HW: per-line trace of CRTC, Gate Array and ASIC writes for a chosen
frame (used with the headless host's TRACE_AT); PAL_COUNT: palette entries in
use. Both compile to nothing unless defined."

post HEAD arnold/src/cpc/diskimage/iextdsk.c arnold/src/cpc/fdc.c
commit "Disk images: fix the DSK and EDSK loaders" "EDSK: the loader stepped and copied each sector by the larger of its stored
length and its ID's size (N), so a short sector, such as the common N=6
protection holding 6144 bytes, read past the image buffer (AddressSanitizer)
and every later sector of the track got the wrong data. It now steps by the
stored length and clamps the copy. Tracks with more than 29 sectors use a
512-byte track header, as CPCEC writes them.
Standard DSK: every sector occupies the track header's sector size (as CPCEC),
not its ID's size. Also an off-by-one in the sector index check, pointers left
dangling after a format, and a leftover debug printf in FDC_BeginSeek.
Checked with generated test disks (odd sizes, gaps, weak and deleted sectors,
42 tracks, two sides) and 13 real disks against CPCEC. NO_DSK_FIX restores the
old loaders."

post a45f4cd arnold/src/cpc/render5.c arnold/src/cpc/crtc.c
commit "Paletted renderer: draw the border in the border pen's colour" \
"Render_Paletted_PutBorder (and its inlined copies) took the border colour from
ConvertedColourTable[16], which only the true-colour renderer fills. In the
paletted renderer the border therefore kept whatever colour palette index 0
first held, BASIC's blue, for the rest of the session (R-Type's black border
showed blue). It now uses the palette index the border pen was remapped to.
NO_BORDER_FIX restores the old behaviour."

post 05c93b8 arnold/src/cpc/z80/z80macros.h
commit "Z80: overflow flag on SUB, SBC, CP and SBC HL" \
"For A - r the overflow flag must be (A^r)&(A^result)&0x80; the core used
(r^A)&(r^result), the exact opposite whenever the operands' signs differ, so V
was wrong in every case that matters (SBC HL the same). Old games rarely look
at V after a subtraction, but compiled code does for every signed compare
(SDCC: sub/sbc; jp po; xor 80h; jp m). Puzzle Bobble (2021 cartridge) froze at
the start of round 1: its aiming-line routine never ended. Now matches CPCEC
frame for frame; seven other homebrew carts change slightly in timing, all
checked against CPCEC. NO_Z80V_FIX restores the old flag."

post e40ded1 arnold/src/cpc/render5.c
commit "Paletted renderer: free the palette when it is nearly full" \
"The paletted renderer gives each new colour one of 256 host palette entries
and never frees them (the Palette_Reset in Render_DumpDisplay is commented
out). Plus games use many of the 4096 colours (Sonic GX: 200 entries in 30 s),
so after a game or two the palette is full, new colours are dropped and drawn
in a stale entry's colour. At 224 entries the palette is now freed at the end
of a frame and the next frame takes entries for the colours it uses. Only the
palette numbering changes: the RGB output is identical on every cartridge this
affects. NO_PALGC_FIX restores the old behaviour."

post e40ded1 arnold/src/cpc/cpc.c
commit "CPC_PowerCycleMemory: what a power cycle clears and a reset does not" \
"Clears main RAM, expansion RAM and the ASIC's RAM (to its start-up value), for
a host's Power ON, so a game starts as it does from cold whatever ran before.
Not called by the core itself."

post a865618 arnold/src/cpc/render5.c
commit "Paletted renderer: keep palette entry 0 black" \
"The paletted renderer draws blanking with entry 0 (Render_PaletteIndexBlack)
and assumed it black, but entry 0 went to whichever colour was stored first.
After the palette was freed (a host's power cycle, or freeing when nearly
full) a game's first colour, red in Ghosts'n Goblins, took entry 0 and filled
every blank area; on an Amiga entry 0 also fills the screen surround. Entry 0
is now reserved as black. Palette numbering changes only: RGB output identical.
NO_BLACK0_FIX restores the old behaviour."

post e7d179a arnold/src/cpc/asic.c arnold/src/cpc/cpc.c
commit "Plus: IM2 vector worked out at interrupt acknowledge" \
"On the Plus the ASIC puts the vector of the interrupt being acknowledged on the
bus (raster/PRI or the Gate Array's 52-line interrupt, else DMA 2/1/0). Arnold
set the vector only when a DMA or PRI interrupt was requested, so a 52-line
interrupt (PRI 0) reused the last DMA vector and ran the DMA handler. The vector
is now computed at acknowledge time, before the source is cleared, as CPCEC does.
Also, a write to the DMA control register (&6C0F) no longer cancels a pending
raster interrupt. Sonic GX's title and demo levels chain one DMA interrupt per
character row to re-aim the split screen; the extra handler runs moved its table
on and scrambled them. Now matches CPCEC frame for frame; no other homebrew cart
changes. NO_IVEC_FIX restores the old code."

post bbf17a9 arnold/src/cpc/z80/z80macros.h
commit "Z80: NO_Z80V_FIX builds again" "The switch's macro needs the operand passed in."

post 69fed41 arnold/src/cpc/asic.c arnold/src/cpc/asic.h arnold/src/cpc/cpc.c arnold/src/cpc/crtc.c
commit "Plus: hold the interrupt request while ASIC interrupts are pending; stagger DMA INTs" \
"Two Plus interrupt fixes, as CPCEC models them. (1) After acknowledging one ASIC
interrupt, the Z80's request was cleared even when another (e.g. DMA 0 after
DMA 1) was still pending, so that one was taken a line late at the next HSYNC.
The request is now held while any ASIC interrupt is pending; a DCSR write that
clears the last one drops it. NO_INTHOLD_FIX. (2) The DMA channels ran at the
same instant; the ASIC runs them one after another, so a channel's INT is now
raised 1 us after each lower channel that ran a command on that line.
NO_DMA_STAGGER. Sonic GX restarts its DMA lists from the DMA 0 interrupt; when
DMA 1's interrupt landed on the same line the restart slipped and its split
screen interrupt came too late (stacked bands in Green Hill), and its special
stage froze. Both now match CPCEC; no other homebrew cart changes."

post 29464ee arnold/src/cpc/render5.c
commit "Paletted renderer: keep the palette below 64 entries" "The paletted renderer freed its palette only at 224 entries, so a Plus game's
colours spread over the whole 0-255 range and a slow host (Amiga chunky-to-
planar) had to convert all 8 bitplanes. The palette is now compacted at the
end of a frame as soon as an entry of 64 or above is in use (PALGC_TOP): the
pens get entries again in pen order, with exactly the colours they have now
(not recomputed, as a Plus pen last set through the Gate Array has a slightly
different RGB from its ASIC colour). Sonic GX uses about 57 colours a frame,
so its pictures fit in 6 bitplanes. Palette numbering changes only: RGB output
identical. NO_PALGC_LOW restores the 224-entry limit."

post 851f65a arnold/src/cpc/render5.c
commit "Plus: table-driven horizontal soft scroll" "The per-pixel shift in Render_ScrollGraphicsData moves each input bit to a
fixed place, so its result is the OR of what each input byte gives alone.
Render_ScrollTable builds four 256-entry lookups per mode (0, 1) and scroll
(1-7) on first use; the shift is now four lookups. Checked against the loop on
280 million random inputs. NO_SCROLL_TABLE restores the loop."

post 851f65a arnold/src/cpc/crtc.c arnold/src/cpc/arnold.c arnold/src/cpc/cpc.c arnold/src/cpc/z80/z80.c
commit "CRTC: defer quiet cycles and draw them in one run" "The CRTC was stepped after every Z80 instruction, for 1-6 cycles, and setting
up those tiny runs cost more than drawing them. Cycles that don't reach the
next CRTC event are now only counted (CRTC_Pending) and drawn later in one run:
before the next event cycle, before anything that can change what they draw
(every CRTC_InvalidateRun caller, Z80 memory writes, IN) and at frame end, so
they see exactly the memory and state they did before. The run loop also uses
the soft-scroll table directly. Pixel-exact on the test set; with the scroll
table, Sonic GX's core time on a 68040 (PiStorm) fell from 14.9 to 12.9 ms a
frame. NO_DEFER restores per-instruction stepping."

post 1b5dc34 arnold/src/cpc/asic.c
commit "Plus: skip transparent sprite rows" "A hardware sprite row whose 16 pixels are all pen 0 is skipped, and the pixel
loop stops at the sprite's edge. Pixel-exact; about 11% less sprite time.
NO_SPRROW restores the full loop."

post HEAD arnold/src/cpc
commit "Core: remaining changes" "Small changes made after the fixes above."

post HEAD arnold/src
rm -rf $WT/src/headless
commit "AmigaOS host (68k AGA): Arnold for Amiga" \
"src/amiga: the AmigaOS 3.x host (AGA screen with chunky-to-planar and triple
buffering, Paula sound, rawkey keyboard matrix, joystick/CD32 pads and keys as
joystick, GadTools menu, prefs and per-game settings, Workbench icons and
presets, screenshots, key scripts for test runs). Built with bebbo's
amiga-gcc (-noixemul): see src/amiga/Makefile."

post HEAD arnold/src/headless
commit "Headless test host" \
"src/headless: runs the core without a display (renders N frames to a buffer,
prints a checksum, writes a PPM; optional key script, frame-skip hook test and
WAV output). Used for the pixel-exact checks of every change above."

echo "--- remaining diff vs HEAD:arnold/src:"
git diff --stat upstream-patches:src HEAD:arnold/src | tail -3
