#!/bin/bash
# Build the upstream patch series on branch upstream-patches (worktree build/upstream-wt).
# Setup: git worktree add build/upstream-wt -b upstream-patches e5dce08
# Then: git format-patch -o <dir> e5dce08..upstream-patches. hscroll.py / prifix.py
# replay the two Plus fixes so they become separate patches.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
PKG="$R/tools/package"
WT=$R/build/upstream-wt
B13=d9af72a   # main after 1.3: the core of 1.3 (the steps below that once took HEAD take this, so 1.4 changes stay out of them)
cd ${SERIES_TREE:-$R}   # HEAD of this tree is what the series is made from (a release worktree: SERIES_TREE=<its path>)
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

post $B13 arnold/src/cpc/diskimage/iextdsk.c arnold/src/cpc/fdc.c
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

post 72894cd arnold/src/cpc/asic.c
commit "Plus: merge only the sprite pixels a lores host reads" "Two exact speed-ups of the Plus sprite merge for a host that renders every
other pixel (PIXEL_STEP 2, e.g. an Amiga lores screen). (1) Only the 8 even
pixels of a column are merged; each gets the same pixel as before (the first
opaque sprite in priority order), and the odd mask bits, which nothing reads
at PIXEL_STEP 2, come back as no sprite. NO_SPREVEN restores the full loop.
(2) ASIC_BuildColMask also records which sprites cover each column, so the
merge visits only those (about 3) instead of testing all 16. NO_SPRCOL.
SPR_PAD (default 72 bytes, NO_SPRPAD: none) pads the code here: the Emu68 JIT
on a PiStorm runs the core up to 7% faster or slower with the code layout.
Pixel-exact (headless checksums of the test set and whole-run frame
checksums); on an A1200 PiStorm Sonic GX's spring frames lose about 2.4 ms."

post bb00f26 arnold/src/cpc/arnold.c arnold/src/cpc/cpc.c arnold/src/cpc/z80/z80.c arnold/src/cpc/z80/z80.h arnold/src/cpc/z80/z80funcs.h
commit "CRTC: register writes at their I/O cycle, not at the start of the OUT" "The CRTC was run after each whole Z80 instruction, so an OUT reached it at the
start of the instruction, 2-4 us before its I/O cycle on the CPC. Pinball
Dreams rewrites R0/R4/R5/R7/R9 several times a frame with 1-line rows; its R0
write landed before the end of the line it was meant to follow, HCC ran on to
255 and the frame lost sync (rolling picture, then a crash at ball launch).
Z80_IO_AT(n) before each OUT/IN: the host's CPC_IOCatchUp runs the CRTC to the
I/O cycle (CPCEC's NOP positions), the rest of the instruction afterwards.
Only CRTC register writes are timed by default; IOTIME_ALL times every port.
NO_IOTIME_FIX restores I/O at the start of the instruction."

post 1eacab0 arnold/src/cpc/asic.c arnold/src/cpc/audioevent.c arnold/src/cpc/psgplay.c
commit "Sound: AY rendering closer to the chip" "Measured against a reference AY render (and CPCEC): tones were point-sampled
(aliasing), hardware envelopes ran an octave high and one-shot shapes
restarted every 32 steps, the noise clocked at twice its rate, levels 0-2 and
3-5 collapsed, tones were bipolar (6 dB louder than volume-register sound),
the 16-bit conversion indexed a 32-entry table with the mixed byte (crackle on
the loud negative peaks) and the first block was uninitialised. Each fix has
its switch: NO_SNDAA (sub-sample averaging), NO_SNDENV_FIX, NO_SNDNOISE_FIX,
NO_SNDLEVELS (0..124 levels), NO_SNDUNIPOLAR, NO_SNDLUT_FIX."

post c0b5bd8 arnold/src/cpc/asic.c arnold/src/cpc/asic.h arnold/src/cpc/cpc.c arnold/src/cpc/crtc.c arnold/src/cpc/z80/z80.c arnold/src/cpc/z80/z80.h arnold/src/cpc/z80/z80macros.h
commit "Plus: ASIC register writes at their write cycle, DMA at HSYNC start" "Memory-mapped ASIC writes (&6000-&7FFF and sprite pixels &4000-&4FFF) now
take effect at the instruction's memory-write NOP (Z80_WriteNop, CPCEC's
positions; PUSH writes the high byte first) instead of its start, like the
CRTC writes. The Plus DMA runs at the start of HSYNC with its interrupt
DMAPHASE_DELAY cycles later, not at the end of HSYNC: Sonic GX's interrupt
code ran about 10 characters late against the beam. NO_ASICTIME,
NO_ASICTIME_SPR, NO_DMAPHASE restore the old timing."

post a9048c3 arnold/src/cpc/arnold.c arnold/src/cpc/asic.c arnold/src/cpc/asic.h arnold/src/cpc/cpc.c arnold/src/cpc/crtc.c arnold/src/cpc/hint.h arnold/src/cpc/render5.c arnold/src/cpc/z80/z80.c
commit "Speed-ups for the Emu68 JIT (pixel-exact); I/O lead carried over" "Emu68 (PiStorm) guesses every branch at translation time (forward = not
taken, backward = taken only within 128 bytes) and a wrong guess leaves the
translated block. Hot paths are shaped for that: Plus cells and border cells
with the visible range worked out once and short loops (NO_EMUBR), sprite
cells inline (NO_SPRINL) and each line's sprite pixels composed once
(NO_SPRLINE), branch hints in the main loop (NO_HINTS), each Z80 opcode
ending with its own interrupt check (NO_Z80TAIL). Pixel-exact (headless
checksums, whole-run frame checksums). Also: when an I/O or ASIC write is
timed later than Arnold's instruction is long (LD (HL),n), the CRTC's lead is
carried into the next instruction instead of a negative cycle count
(NO_IOAHEAD_DEBT restores that)."

post a9048c3 arnold/src/cpc/fdd.c arnold/src/cpc/fdd.h arnold/src/cpc/diskimage/diskimg.c
commit "Disk drives: an 80-track image gets an 80-track drive" "Drive A is a 43-track 3\" drive, so the head stopped at track 42 on 80-track
(3.5\") images: Pinball Dreams' Beat Box and Nightmare tables never loaded
(CPCEC loads them). FDD_FitDiskTracks, called when a .dsk or extended .dsk is
inserted: an image with more tracks than the drive gets an 80-track drive (up
to 86 tracks), an image that fits keeps the drive's own count (A 43, B 80).
NO_DSK80 restores the old behaviour."

post 5b15ae7 arnold/src/cpc/diskimage/diskimg.c arnold/src/cpc/diskimage/dskwrite.c arnold/src/cpc/diskimage/dskwrite.h arnold/src/objects.mak
commit "Disk images: write changes back to the .dsk file" "dskwrite.c builds a new image for a drive from the file it was loaded from and
the drive's changes: unchanged tracks are copied from the file byte for byte
and only changed sectors patched in, so a standard DSK stays standard where it
can and weak sectors, gaps and odd stored lengths are kept. It saves safely:
name.tmp first, the original kept once as name.bak, then the rename. A host
calls DskWrite_Flush, e.g. on eject, disk swap, Power ON, the menu and exit.
DiskImage_GetUnitPointer gives it the drive's internal image. Nothing changes
unless a host calls it."

post 1943fe5 arnold/src/cpc/crtc.c arnold/src/cpc/render5.c
commit "Monitor: 62-character line threshold and a horizontal flywheel" "Pinball Dreams' 21st Century logo alternates 66- and 62-character lines. The
monitor emulation started a new line only after 63 characters past hsync, so
pairs of lines merged and the logo was half its height (106 lines against
CPCEC's 226). A new monitor line now starts at 62 (NO_MONLINE_FIX keeps 63).
Horizontal flywheel: a real monitor's line oscillator averages the hsync
position, so a per-line hsync change does not shift alternate lines sideways.
A steady hsync gives exactly the old position; a mode change or a short-line
trick locks to the new length. The renderer starts the row the given number
of characters off its left edge. NO_HFLYWHEEL restores the old placement."

post 5b15ae7 arnold/src/cpc/cassette.h arnold/src/cpc/cpc.c arnold/src/cpc/tzx.h arnold/src/cpc/tzxold.c
commit "Tape: pause timing at 4 MHz; accessors for a host" "The TZX player scales pulse lengths to the CPC's 4 MHz, but converted pauses
at the Spectrum's 3500 T-states a millisecond, so every pause was 12.5% short.
Pauses are now 4000 T-states a millisecond (NO_TAPE_FIX restores the old
figure). With it, a tape's motor time matches the file to 0.2 s. For a host:
CPC_GetTapeMotor (the cassette motor bit), TapeImage_IsPlayable (a tape is in
and has blocks left), and TapeImage_SetBlockByIndex declared in tzx.h."

# 1.1 (26 Oct 2026): the code review's write-back fixes, then the tape sound
post 1b4ad2c arnold/src/cpc/diskimage/dskwrite.c
commit "Disk write-back: read a track's sector IDs only inside its block" "An extended track may claim 30-61 sectors in a 256-byte track block (the loader
accepts it). The write-back read that many sector IDs from the track header,
up to 248 bytes past the block, and past the end of the file for the last
track. The ID count is now limited to what fits in the block; such a track
then differs from the drive's copy and is written from it."

post 392276c arnold/src/cpc/diskimage/dskwrite.c arnold/src/cpc/diskimage/dskwrite.h
commit "Disk write-back: refuse when the temp or backup name would clash" "The write-back uses <name>.tmp, <name>.bak and <name>.old. A volume that cuts
file names to 30 characters (OFS/FFS of older Kickstarts) makes these one file
for a 29-character name, and .tmp the image itself for a 30-character one: the
write was lost, or the disk file deleted. Two new host hooks, Host_SameFile
(the same file, e.g. SameLock) and Host_NameKept (stored under the full name),
let DskWrite_SaveFile refuse before anything is removed or renamed; the
changes stay in the drive and the host is told why."

post $B13 arnold/src/cpc/cassette.c arnold/src/cpc/cassette.h
commit "Tape: the loading sound, for a host to mix in" "The tape signal as sound, as CPCEC does it. While a host sets TapeAudio_On,
Cassette_Read notes each change of the level it hands the Z80 with its NOP
time, and TapeAudio_Mix adds the frame's level to the host's finished 16-bit
sample block: a step of 1/8 of the loudest mix (CPCEC's level), averaged over
each sample and low-passed at about 5 kHz. The player only moves when the Z80
reads the port, so a loader that stops reading is silent for that stretch.
Off (the default), nothing is recorded or mixed and the machine runs exactly
as before; sound never changes the emulation."

# 1.2 (26 Oct 2026): the hires changed-line fix
post $B13 arnold/src/cpc/render5.c
commit "render: the changed-line hook names the surface row in scanline mode" "With ScanLines set (RENDERING_ACCURACY_HIGH, a host's hires picture) CPC line i
is copied to surface row Render_ScreenYOffset + 2i, with the rows between left
black, but Render_LineChangedHook was called with Render_ScreenYOffset + i, so
a host converting only the changed rows redrew the wrong ones and kept stale
lines on screen (BASIC MODE 2 scrolling: 33000 of 36000 changed rows missed).
The hook now gets the row the line was written to. Without ScanLines (the
normal picture) nothing changes, and the pixels drawn are the same either way."

# 1.4 (28 Oct 2026): speed14, then save states
post 466ce0a arnold/src/cpc/arnold.c arnold/src/cpc/asic.c arnold/src/cpc/crtc.c
commit "Speed-ups: no sprite line mask on undrawn lines; I/O lead tested first" "Two exact speed-ups. (1) Only drawing reads the Plus sprites active on a line,
so while a frame is not drawn (frame skip) the mask is no longer worked out at
each line's end; it is marked stale and CRTC_SetRenderState works it out when
drawing comes back on (mid-line, in the lead-in lines a drawn frame redraws).
NO_SKIPSPR restores the per-line rebuild. (2) The I/O lead carried between
instructions (CPC_IOAhead) is rarely there, so it is tested before it is taken
instead of stored after every instruction. NO_IOTAKE_TEST. Pixel-exact
(headless checksums, frame skip on and off, whole-run frame checksums)."

post HEAD arnold/src/cpc arnold/src/objects.mak
commit "Save states: save and load the whole machine exactly" "cpc/savestate.c: SaveState_Save / SaveState_Load / SaveState_Check for a host,
between two CPCEmulation_Run calls. The file is a standard SNA v3 (upstream's
writer: RAM and registers for other emulators) with an extra ARNS chunk: a
versioned, CRC-checked block of sections, one per core file (Z80, CPC, Gate
Array, PAL, ASIC, CRTC, AY, sound events, FDC, drives, disc interface, printer,
tape, renderer, main loop) plus the host's own bytes, all big-endian and of
fixed width, so a state saved on a little-endian PC loads on a 68k Amiga and
back. Every inserted disk is stored whole. Each core file gets an SS_ function
that saves or restores its state (pointers as offsets or indices); caches and
tables are worked out again. A load is checked first (structure, CRC, format,
machine, cartridge, tape) and applied whole: if anything fails part way, the
running machine, kept in memory, is put back. A plain .sna still loads through
Snapshot_Insert, after a bounds check of its chunks. Resumed runs give the same
frames and sound as uninterrupted ones (headless tests: carts, disks, tapes,
frame skip). NO_SAVESTATE builds the core without it."

post HEAD arnold/src/cpc
commit "Core: remaining changes" "Small changes made after the fixes above."

post HEAD arnold/src/amiga/rtgfw
commit "RTG display framework (rtgfw) for the AmigaOS host, MIT licence" \
"src/amiga/rtgfw: rtgfw, a small RTG (Picasso96 / CyberGraphX) display layer
shared with other Amiga emulator ports: opens an RTG screen in a chosen mode
and depth, and writes a chunky frame's changed lines into it (8-bit CLUT,
16 and 32-bit, with optional 2x scanlines). Copyright (c) 2026 Zoopster,
under the MIT licence (src/amiga/rtgfw/LICENSE), not the GPL: it is a
separate work, vendored unchanged from its own repository (SOURCE.txt names
the commit). Only the AmigaOS host uses it; the core does not."

post HEAD arnold/src
rm -rf $WT/src/headless
commit "AmigaOS host (68k AGA): Arnold for Amiga" \
"src/amiga: the AmigaOS 3.x host (AGA screen with chunky-to-planar and triple
buffering, an optional RTG screen through rtgfw, Paula sound, rawkey keyboard
matrix, joystick/CD32 pads and keys as joystick, GadTools menu, prefs,
per-game settings and a game settings database (Games.db), Workbench icons
and presets, screenshots, key scripts for test runs). Built with bebbo's
amiga-gcc (-noixemul): see src/amiga/Makefile."

post HEAD arnold/src/headless
commit "Headless test host" \
"src/headless: runs the core without a display (renders N frames to a buffer,
prints a checksum, writes a PPM; optional key script, frame-skip hook test and
WAV output). Used for the pixel-exact checks of every change above."

echo "--- remaining diff vs HEAD:arnold/src:"
git diff --stat upstream-patches:src HEAD:arnold/src | tail -3
