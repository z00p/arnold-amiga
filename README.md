# Arnold for Amiga

Amstrad CPC 6128 Plus / GX4000 and CPC 464/664/6128 emulator for AGA Amigas with a 68040 or better: an A1200
with a PiStorm32 (Emu68), 68060 cards, or BigMig on MiSTer. It is a port of Kevin Thacker's
**Arnold** to 68k AmigaOS 3.x, with the front end modelled on AmiMSX. GPL-2.0.

Arnold is (c) Kevin Thacker. The port starts from rofl0r's GitHub mirror of Arnold
(https://github.com/rofl0r/arnold, master at e5dce08). Kevin has given the port his blessing
and asked for it to keep the Arnold name.

## What works

- CPC Plus / GX4000 cartridges (.cpr), disks (.dsk and extended .dsk, with BASIC and AMSDOS
  from the Plus system cartridge) and snapshots (.sna).
- Models: CPC 464, 664 and 6128 (booting to BASIC from their own system ROMs), 6128 Plus and
  GX4000, picked in the menu's Settings box.
- PAL and NTSC: Video mode Auto/PAL/NTSC; on a 60 Hz NTSC display the CPC still runs at its
  own 50 Hz (frames timed from the E-clock, Paula at the matching rate), or follows the display
  if you prefer. The menu fits a 200-line NTSC screen.
- Full speed with sound on a PiStorm A1200, including Sonic GX at 50 fps without frame skip
  (see What to expect); the hot code is shaped for Emu68 and built with a recorded profile. Fast vertical scrolling moves the picture in a taller chip RAM buffer
  (about 0.5 MB more chip RAM; `NOVSHIFT` turns it off). Paula sound, 8-bit stereo. Frame skip for slower machines, with every
  drawn frame exact.
- Menu on Help: media, sound, screen mode, full height (`TALL`, all 272 lines), wide (`WIDE`, 362 of the 384 columns), frame skip,
  joystick-from-keyboard, per-game settings (also for disk games), keyboard map, IFF and PNG
  screenshots. Workbench presets with ToolTypes. Per-game key remap (`REMAP=RSHIFT:M`; a
  remapped key gives only its new key, so Right Shift then no longer works as Shift), with a
  key-map editor and presets in the Controls menu.
- Joystick in port 2, two-button and CD32 pads, keyboard as joystick (joy keys mode 3: X / Space
  are fire 1, for R-Type's beam).
- RTG screen (experimental, `RTG`): on an RTG board with an 8-bit mode (the PiStorm's VideoCore,
  WinUAE's uaegfx) the emulation and the menu show on an RTG screen instead of AGA, with optional
  scanlines (`SCANLINES=`: Hard 25/50/75%, Blended light/normal/strong; on a PiStorm at 960x540,
  exactly half of its 1080p output). The "Arnold RTG" preset starts there with Hard 50%; N in the
  menu cycles the scanline options. Frames are written just after the display's vertical blank, so
  there is no tearing (`RTGVBL=0` for the old immediate writes). Off by default: the AGA screen is
  unchanged.
- Game settings database (`Games.db` in the drawer): settings for known games (wide, full height,
  key remap, joy keys), found by the CRC-32 of the cartridge, disk or tape; a game's own
  Save for game settings always win.

Disks: changes a program makes (SAVE, high scores, FORMAT) are written back to the .dsk
file, with the original kept once as `<name>.dsk.bak` (option "Write disks back",
`DISKWRITE=0` to turn it off).

Save states (1.4): four slots per game (Project menu, or Left Amiga + F1-F4 to save and Right
Amiga + F1-F4 to load while playing), in `PROGDIR:States/<game>.<slot>.sna`. A state holds the
whole machine, and the game goes on exactly where it was, to the frame and to the sample (proved
by an automated test that compares every frame and the sound of a resumed run with an
uninterrupted one). The files are standard .sna v3 snapshots with an extra Arnold block (other
emulators can read the RAM and registers). Every inserted disk is kept in the state, so a state
never needs a disk; a state for another cartridge or model is refused with a message and nothing
changes. The hotkeys show a short message ("Saved 1", "Loaded 1" or why not) for about 2 s.

Tapes: .cdt (TZX) images load on every model, with fast tape loading (`FASTTAPE`) that leaves
the machine exactly as at real speed, and an optional tape loading sound at real speed
(`TAPESOUND`, off by default). Auto-start tapes (1.5; Options menu, `AUTOTAPE=0|1`, on by
default): after Power ON Arnold types the load command (`RUN"` on a 464, `|TAPE` and `RUN"` on
a 664 or 6128, f1 first on the Plus) and presses a key at "Press PLAY then any key". Saving to
tape is not supported, and only .cdt images load (no .wav, .voc or .csw samples).

Fast-forward (1.5): hold the ` key (left of 1) and the CPC runs as fast as the Amiga allows: no
50 Hz wait, about 10 pictures a second, no sound, and a ">>" symbol at the top left. The emulation
is exactly the same as at normal speed (only what is shown and heard changes), and on release it
goes straight back to real time with no catch-up.

Black (1.5): an Amiga's colour 0 also fills the screen surround in the border colour, so black
pens no longer share palette entry 0 (black parts of a picture vanished on a non-black border:
R-Type's Easter Egg logo). The on-screen messages use a pen below 128, so they show on 8-bit RTG
screens with scanlines too. Known issues are listed in the drawer's README.txt
(NTSC cropping of edge-drawn games).

Speed (1.4): lines that are not drawn (frame skip) no longer work out their Plus sprites, and the
I/O timing lead is tested before it is taken; both exact (every frame unchanged).

AGA display (1.3): only the bitplanes the picture uses are shown (chip RAM writes are faster, `NOBPU`
turns it off), and the once-a-second log lines are kept in memory and written when the display
closes (`HEARTBEAT` writes them at once). On a PiStorm, Sonic GX frames over 20 ms dropped from
15-19 to about 2 per run. `CHIPBENCH` times chip RAM writes (writes `chipbench.txt`).

## What to expect

Arnold for Amiga targets PiStorm/Emu68-class machines.

- **A1200 with a PiStorm32 CM4:** the games tested run at full speed (Sonic GX at 50 fps without
  frame skip, measured, springs included). On a real A1200 Sonic GX uses about 60% of the frame
  time. Use `Arnold` or the PiStorm preset.
- **BigMig (MiSTer):** games currently run below full speed; use `Arnold-060` with frame skip (the BigMig
  preset, which uses BigMig's RTG screen: faster than AGA there).
- **Real 68060 accelerator cards** have not been tested; expect games below full speed.
- **68030 machines** are too slow for games (`Arnold-030` is for trying BASIC).

## Download

The release zip holds the Amiga drawer (five builds: 68040 and 68060, each at -O2 and -O3,
and a 68030 build; all soft-float), icons, presets, the Plus system cartridge, the CPC 464/664/6128 system ROMs
and a user README. See the
Releases page. The RTG code uses the RTG display framework (rtgfw, MIT licence; its
licence text is `LICENSE-rtgfw` in the package and `arnold/src/amiga/rtgfw/LICENSE` here).

## Core fixes

The port found and fixed bugs in Arnold's core; they are in `patches/` as a series against
upstream, one patch each, so they can be taken independently:

- Plus horizontal soft scroll in modes 0 and 1 (the paletted renderer mixed neighbouring
  pixels' bits: Sonic GX scrambled, Ghosts'n Goblins GX at times).
- The programmable raster interrupt raised at HSYNC start, as on CPCEC (Sonic GX's split
  screens and palette changes).
- The 16-bit audio conversion (half of each block played at half speed).
- Frame skip restarting drawing mid-line.
- The DSK and EDSK loaders reading past short sectors (the N=6 / 6144-byte protection).
- The border in the paletted renderer, stuck at BASIC's blue for the whole session.
- The Z80's overflow flag after subtractions (compiled games' signed compares; Puzzle Bobble froze).
- The paletted renderer's 256 colours, never freed (corruption after a game or two).
- The Plus interrupt vector for the Gate Array's 52-line interrupt (Sonic GX's title and demo levels).
- Plus DMA interrupt handling (Sonic GX's Green Hill split glitch and frozen special stage).
- CRTC register writes at their I/O cycle instead of the start of the OUT instruction
  (Pinball Dreams lost its frame sync and crashed).
- Sound: the AY rendered with averaging (no aliasing), the envelope rate and hold shapes,
  the noise rate, the volume levels and unipolar output, the 16-bit lookup (crackle).
- Plus timing: ASIC register writes at their write cycle, DMA at the start of HSYNC.
- 80-track disk images: drive A was always a 43-track 3" drive, so 3.5" images stopped at
  track 42 (Pinball Dreams' Beat Box and Nightmare tables never loaded).

Plus pixel-exact speed-ups for slow hosts (cell caching, run-based and deferred CRTC stepping,
HALT fast-forward, a compact palette, table-driven Plus soft scroll, transparent sprite rows
skipped, Plus sprites merged only where they cover the line and only at the pixels drawn), each with a `NO_x` define to switch it off.
`patches/README.txt` has the details.

## Layout

| Path | What |
|---|---|
| `arnold/src/cpc` | Arnold core with the port's changes (each marked "Amiga port") |
| `arnold/src/amiga` | AmigaOS host: AGA screen and c2p, RTG screen, Paula sound, menu, prefs, Games.db, Workbench, key scripts |
| `arnold/src/amiga/rtgfw` | The RTG display framework (vendored, MIT) |
| `arnold/src/headless` | Headless x86 host for pixel-exact checks, disk tests and WAV output |
| `patches/` | The changes as a patch series against upstream Arnold |
| `tools/` | Build and check scripts (below) |
| `tests/`, `run/Keys/` | Key scripts for the test runs |
| `winuae/` | WinUAE configs and driving scripts for the regression runs |

## Building

Amiga: bebbo's amiga-gcc (https://codeberg.org/bebbo/amiga-gcc) in `/opt/amiga`, then

    cd arnold/src/amiga && make TARGET=Arnold CPU=68040 OPT=-O2

or `bash tools/build_variants.sh` for all five builds.
The release binaries are also built with `-fno-function-cse -fno-unroll-loops -fno-peel-loops`
and a profile recorded in WinUAE (gcc `-fprofile-use`); without the profile the code is
the same, only laid out less well for Emu68 (Sonic GX about 2 ms a frame slower on a PiStorm).

PC checks need Linux with gcc (WSL works). `tools/headless_check.sh` compares pixel-exact
frame checksums of the working tree against a reference commit for every cartridge in
`run/carts`; every core change must keep it at "0 different". `tools/headless_skipcheck.sh`
checks that frame skip never changes a drawn frame; `tools/dsk_tests.py` tests the disk path
with generated disks. The test cartridges are not in the repo: the checks use the system
cartridge and whatever you put in `run/carts`.

## Credits

Arnold by Kevin Thacker. Amiga port by John Stringer, 2026. The GX4000 pixel-art icon and
the menu are the port's own. CPCEC by CNGSoft was the accuracy reference for the Plus fixes.
