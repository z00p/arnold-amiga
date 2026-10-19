# Arnold for Amiga

Amstrad CPC 6128 Plus / GX4000 emulator for AGA Amigas with a 68040 or better: an A1200
with a PiStorm32 (Emu68), 68060 cards, or BigMig on MiSTer. It is a port of Kevin Thacker's
**Arnold** to 68k AmigaOS 3.x, with the front end modelled on AmiMSX. GPL-2.0.

Arnold is (c) Kevin Thacker. The port starts from rofl0r's GitHub mirror of Arnold
(https://github.com/rofl0r/arnold, master at e5dce08). Kevin has given the port his blessing
and asked for it to keep the Arnold name.

## What works

- CPC Plus / GX4000 cartridges (.cpr), disks (.dsk and extended .dsk, with BASIC and AMSDOS
  from the Plus system cartridge) and snapshots (.sna).
- Full speed with sound on a PiStorm A1200: 39-58% of the frame budget in the games measured.
  Paula sound, 8-bit stereo. Frame skip for slower machines, with every drawn frame exact.
- Menu on Help: media, sound, screen mode, frame skip, joystick-from-keyboard, per-game
  settings, keyboard map, IFF and PNG screenshots. Workbench presets with ToolTypes.
- Joystick in port 2, two-button and CD32 pads, keyboard as joystick.

Not yet: writing changed disks back to the .dsk file (SAVE lasts until Power ON or Exit),
tape, the plain CPC 464/664/6128 (the port is Plus only so far). Known issues are listed in
the drawer's README.txt (Sonic GX speed, Pinball Dreams, NTSC speed).

## Download

The release zip holds the Amiga drawer (four builds: 68040 and 68060, each at -O2 and -O3,
all soft-float), icons, presets, the Plus system cartridge and a user README. See the
Releases page.

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

Plus pixel-exact speed-ups for slow hosts, each with a `NO_x` define to switch it off.
`patches/README.txt` has the details.

## Layout

| Path | What |
|---|---|
| `arnold/src/cpc` | Arnold core with the port's changes (each marked "Amiga port") |
| `arnold/src/amiga` | AmigaOS host: AGA screen and c2p, Paula sound, menu, prefs, Workbench, key scripts |
| `arnold/src/headless` | Headless x86 host for pixel-exact checks, disk tests and WAV output |
| `patches/` | The changes as a patch series against upstream Arnold |
| `tools/` | Build and check scripts (below) |
| `tests/`, `run/Keys/` | Key scripts for the test runs |
| `winuae/` | WinUAE configs and driving scripts for the regression runs |

## Building

Amiga: bebbo's amiga-gcc (https://codeberg.org/bebbo/amiga-gcc) in `/opt/amiga`, then

    cd arnold/src/amiga && make TARGET=Arnold CPU=68040 OPT=-O2

or `bash tools/build_variants.sh` for all four builds.

PC checks need Linux with gcc (WSL works). `tools/headless_check.sh` compares pixel-exact
frame checksums of the working tree against a reference commit for every cartridge in
`run/carts`; every core change must keep it at "0 different". `tools/headless_skipcheck.sh`
checks that frame skip never changes a drawn frame; `tools/dsk_tests.py` tests the disk path
with generated disks. The test cartridges are not in the repo: the checks use the system
cartridge and whatever you put in `run/carts`.

## Credits

Arnold by Kevin Thacker. Amiga port by John Stringer, 2026. The GX4000 pixel-art icon and
the menu are the port's own. CPCEC by CNGSoft was the accuracy reference for the Plus fixes.
