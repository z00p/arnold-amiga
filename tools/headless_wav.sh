# Sound check: record each test title's audio (core + the Amiga host's 8-bit conversion) to build/wav/*.wav
export PATH=/usr/local/bin:/usr/bin:/bin
A=${A:-$(cd "$(dirname "$0")/.." && pwd)}
mkdir -p $A/build/wav
rm -rf /opt/src/hw && mkdir -p /opt/src/hw && rsync -a --exclude 'obj-*' --exclude 'Arnold*' --exclude 'AmiCPC*' $A/arnold/src/ /opt/src/hw/src/
cd /opt/src/hw/src
SRCS="$(tr -d '\r' < objects.mak | sed -n '/^CPC_O=/,/^$/p' | tr -d '\' | sed 's/CPC_O=//;s/\.o/.c/g') headless/host_headless.c amiga/keyscript.c"
gcc -O2 -std=gnu89 -fcommon -I. -DUNIX -DCPC_LSB_FIRST -DCPC_NODEBUGGER -w -o hd $SRCS -lm 2>&1 | grep -v warning | head -5
w() { cp $1 /tmp/w.cpr; ./hd $3 /tmp/w.ppm /tmp/w.cpr 8 $2 0 $A/build/wav/$4.wav; }
w $A/arnold/roms/cpcplus/system.cpr $A/tests/keys/burnin.keys 1000 burnin_rubber
w $A/run/carts/PuzzleBobble.cpr $A/tests/keys/generic.keys 1000 puzzle_bobble
w $A/run/carts/GNGGX.cpr $A/tests/keys/generic.keys 1500 gng_gx
w $A/run/carts/HeadOverHeels.cpr $A/tests/keys/hoh.keys 1500 head_over_heels
ls -la $A/build/wav
