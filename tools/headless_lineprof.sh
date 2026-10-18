# Line-level x86 profile (cachegrind, -O2 -g) of the working tree. MODE=-1 profiles with the
# frame-skip hook (mostly skipped frames), MODE=0 draws every frame. Writes build/profile/lines-<mode>.txt
export PATH=/usr/local/bin:/usr/bin:/bin
A=${A:-$(cd "$(dirname "$0")/.." && pwd)}; MODE=${MODE:-0}
rm -rf /opt/src/lp && mkdir -p /opt/src/lp && rsync -a --exclude 'obj-*' --exclude 'Arnold*' --exclude 'AmiCPC*' $A/arnold/src/ /opt/src/lp/src/
cd /opt/src/lp/src
SRCS="$(tr -d '\r' < objects.mak | sed -n '/^CPC_O=/,/^$/p' | tr -d '\' | sed 's/CPC_O=//;s/\.o/.c/g') headless/host_headless.c amiga/keyscript.c"
gcc -O2 -g -std=gnu89 -fcommon -I. -DUNIX -DCPC_LSB_FIRST -DCPC_NODEBUGGER -w -o hd $SRCS -lm 2>&1 | grep -v "unescaped" | head -3
cp $A/run/carts/HeadOverHeels.cpr /tmp/lp.cpr
valgrind --tool=cachegrind --cache-sim=no --cachegrind-out-file=/tmp/cg.lp ./hd 600 /tmp/lp.ppm /tmp/lp.cpr 8 $A/tests/keys/hoh.keys $MODE >/dev/null 2>&1
cg_annotate --show-percs=yes /tmp/cg.lp cpc/crtc.c cpc/arnold.c 2>/dev/null > $A/build/profile/lines$MODE.txt
grep -E "^ *[0-9,]+ \( *[0-9.]+%\)" $A/build/profile/lines$MODE.txt | sort -t'(' -k2 -rn | head -5 >/dev/null
echo done
