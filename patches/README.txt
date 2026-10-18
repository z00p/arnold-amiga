Arnold for Amiga 0.5 - patch set and Amiga build
=================================================
19 October 2026.  Arnold (c) Kevin Thacker, GPL-2.0.  Amiga port by John Stringer.

This package holds the changes made to Arnold while porting it to 68k AmigaOS 3.x (AGA),
as patches against the upstream source, plus a ready-to-run Amiga build.

  patches/   20 patches (git format-patch) against rofl0r's GitHub mirror of Arnold,
             https://github.com/rofl0r/arnold, master at e5dce08
             ("fix prototype of LoadFile()", 4 Sep 2026)
  amiga/     the Amiga build: drawer Arnold with the program, icons, presets, the Plus
             system cartridge and a user README
  source/    the complete source of this build (the upstream tree with all 20 patches
             applied), for the GPL


Applying the patches
--------------------
  git clone https://github.com/rofl0r/arnold && cd arnold
  git checkout e5dce08
  git am /path/to/patches/*.patch

Each patch stands on its own and builds. Patches 1-18 touch only src/cpc (the core);
19 adds src/amiga and 20 adds src/headless. The core changes are all marked "Amiga port"
in comments, and every behaviour change can be switched off with a define (below).


The patches
-----------
Core, no change to what is emulated:

 0001  Build the core on big-endian hosts (m68k)
       cpcdefs.h defined CPC_LSB_FIRST unconditionally; it is now left to the build
       system (src/Makefile already passes -DCPC_LSB_FIRST).
 0002  render: optional hook telling the host which lines changed
       Render_LineChangedHook; NULL (the default) keeps the old behaviour.
 0003  Speed-ups A-D for slow hosts (pixel-exact)           NO_A, NO_B, NO_C, NO_D
       skip cells outside the visible area; cached pixel longs per cell; per-column
       sprite mask; inline paletted cell path.
 0004  Speed-up E: run-based CRTC stepping, inline Z80 data memory access      NO_E
 0005  Speed-up F: HALT fast-forward (a halted Z80's cycles batched to the
       next CRTC event)                                                         NO_F

Fixes:

 0006  Fix the 16-bit audio output conversion
       AudioEvent_ConvertToOutputFormat_16BitStereo/Mono read the event buffer as
       16-bit words, but it holds 4 bytes a sample. Two samples were consumed per
       output sample: half of each block played at half speed, the rest was silence.
 0007  Frame skip: decide one frame ahead so drawn frames start cleanly
       Drawing was turned back on at the frame boundary, mid-line, so one row of every
       drawn frame was misplaced (FrameSkip has the same flaw). CPC_SkipNextFrameHook
       lets a host skip frames; NULL keeps the old behaviour.
 0008  Plus: horizontal soft scroll in modes 0 and 1                 NO_HSCROLL_FIX
       The paletted renderer shifted the graphics bytes as plain bits, right only in
       mode 2; in modes 0 and 1 it mixed neighbouring pixels' bits (Sonic GX scrambled
       throughout, Ghosts'n Goblins GX at times). Now a per-pixel shift, as the
       true-colour renderer already does.
 0009  Plus: raise the programmable raster interrupt at HSYNC start      NO_PRI_FIX
       It was compared at HSYNC end, ~R3 characters late, and Sonic GX's chain of PRI
       interrupts missed lines (split screens and palette changes stopped). Also raised
       at once when PRI is written with the current line during HSYNC. CPCEC's timing.

 0011  Disk images: fix the DSK and EDSK loaders                         NO_DSK_FIX
       EDSK: sectors were stepped and copied by the larger of their stored length and
       their ID's size. A short sector, like the common N=6 protection holding 6144
       bytes, read past the image buffer and corrupted the rest of the track. Tracks
       with more than 29 sectors now use a 512-byte header. Standard DSK: every sector
       now occupies the track header's size, as in CPCEC. Also fixed: an off-by-one,
       pointers left dangling after a format, and a debug printf in FDC_BeginSeek.

 0012  Paletted renderer: draw the border in the border pen's colour     NO_BORDER_FIX
       The paletted border took its colour from ConvertedColourTable[16], which only
       the true-colour renderer fills, so it kept the colour palette index 0 first
       held (BASIC's blue) for the whole session.
 0013  Z80: overflow flag on SUB, SBC, CP and SBC HL                      NO_Z80V_FIX
       V for A - r was computed as (r^A)&(r^result), the exact opposite of the correct
       (A^r)&(A^result) whenever the operands' signs differ. Old games rarely test V
       after a subtraction, but compiled code does for every signed compare (SDCC:
       sub/sbc; jp po; xor 80h; jp m). Puzzle Bobble froze at the start of round 1.
 0014  Paletted renderer: free the palette when it is nearly full        NO_PALGC_FIX
       Each new colour took one of 256 host palette entries for good (the Palette_Reset
       in Render_DumpDisplay is commented out). Plus games use many colours, so after a
       game or two the palette filled and new colours were drawn in stale entries'
       colours. At 224 entries it is now freed at a frame's end; RGB output unchanged.
 0015  CPC_PowerCycleMemory: clears main, expansion and ASIC RAM, for a host's Power ON
       (a reset alone left the previous game's memory behind). Not called by the core.
 0016  Paletted renderer: keep palette entry 0 black                      NO_BLACK0_FIX
       Blanking is drawn with entry 0, which went to whichever colour was stored first;
       after the palette was freed a game's first colour filled every blank area.
 0017  Plus: IM2 vector worked out at interrupt acknowledge                NO_IVEC_FIX
       A Gate Array 52-line interrupt (PRI 0) reused the last DMA vector and ran the
       DMA handler; a DCSR write also cancelled a pending raster interrupt. Sonic GX's
       title and demo levels (one DMA interrupt per row re-aiming the split) were
       scrambled. Now matches CPCEC; no other homebrew cart changes.
 0018  Z80: NO_Z80V_FIX builds again

Tools:

 0010  Diagnostics for hardware tracing (TRACE_HW, PAL_COUNT; off by default)
 0019  AmigaOS host: Arnold for Amiga (src/amiga, built with bebbo's amiga-gcc)
 0020  Headless test host (src/headless: frame checksums, PPM, key scripts, WAV,
       disk insert / write-back / sector dumps)


How it was tested
-----------------
- Headless (x86 Linux, gcc): every step of the series builds. Frame checksums of the
  Plus system cartridge and four GX4000 homebrew titles after 600 frames are identical
  to unmodified upstream through patches 1-7. Patch 8 changes only Sonic GX, which is
  then correct; patch 12 changes only the border colour. During development, the full checks covered 150 checkpoints on 50
  cartridges, including gameplay driven by key scripts.
- Disks: generated test disks in standard and extended DSK formats. They cover DATA,
  SYSTEM and CP/M boot formats, odd sector sizes, gaps, weak and deleted sectors, an
  unformatted track, 42 tracks and two sides. A Z80 probe on the disk checks the FDC
  directly. Also tested: SAVE and FORMAT with the written-back image checked, and
  13 real disks compared with CPCEC. Clean under AddressSanitizer.
- Puzzle Bobble (patch 13) now matches CPCEC frame for frame into play. Seven other
  homebrew carts change slightly in timing with it; all were checked against CPCEC.
  Patch 14 changes only palette numbering: RGB output identical on every affected cart.
- Frame skip: frames drawn with skipping on are identical to the same frames with
  every frame drawn.
- Sonic GX and Ghosts'n Goblins GX were compared frame by frame with CPCEC (cngsoft).
  Caprice32 and MAME show the same Sonic GX corruption the old code had.
- Amiga: WinUAE regression runs, and real hardware. An A1200 with a PiStorm32 (Emu68)
  runs at full speed using 39-58% of the frame budget with sound. BigMig (MiSTer,
  68060) was also tested.


Amiga build
-----------
See amiga/Arnold/README.txt. In short: CPC 6128 Plus / GX4000 cartridges (.cpr), disks
(.dsk, with the system cartridge's BASIC and AMSDOS) and snapshots (.sna). It needs an
AGA Amiga with a 68040 or better (PiStorm/Emu68, 68060, BigMig), with sound through Paula.
Help opens the menu.

Builds: Arnold (68040, -O2), Arnold-040-Max (-O3), Arnold-060 and Arnold-060-Max
(68060). All are soft-float, with no FPU needed.

Built with: bebbo's amiga-gcc (codeberg.org/bebbo/amiga-gcc), -noixemul;
  cd src/amiga && make TARGET=Arnold CPU=68040 OPT=-O2
