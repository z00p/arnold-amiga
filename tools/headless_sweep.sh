# Compatibility sweep: every cart in run/carts/top80all for 600 and 1500 frames (12 s, 30 s)
# with the generic play script; frames as PPM and the sound as WAV in build/sweep/<n>.*
export PATH=/usr/local/bin:/usr/bin:/bin
A=${A:-$(cd "$(dirname "$0")/.." && pwd)}; O=$A/build/sweep; mkdir -p $O
rm -rf /opt/src/sw && mkdir -p /opt/src/sw && rsync -a --exclude 'obj-*' --exclude 'Arnold*' --exclude 'AmiCPC*' $A/arnold/src/ /opt/src/sw/src/
cd /opt/src/sw/src
SRCS="$(tr -d '\r' < objects.mak | sed -n '/^CPC_O=/,/^$/p' | tr -d '\' | sed 's/CPC_O=//;s/\.o/.c/g') headless/host_headless.c amiga/keyscript.c"
gcc -O2 -std=gnu89 -fcommon -I. -DUNIX -DCPC_LSB_FIRST -DCPC_NODEBUGGER -w -o hd $SRCS -lm 2>&1 | grep -v unescaped | head -3
K=$A/tests/keys/generic.keys
find $A/run/carts/top80all -iname '*.cpr' | sort | while read -r c; do
  n=$(basename "$(dirname "$c")" | cut -c1-3)
  cp "$c" /tmp/sw.cpr
  r1=$(timeout 120 ./hd 600 $O/$n-12s.ppm /tmp/sw.cpr 8 $K 2>&1 | tail -1)
  r2=$(timeout 240 ./hd 1500 $O/$n-30s.ppm /tmp/sw.cpr 8 $K 0 $O/$n.wav 2>&1 | tail -1)
  echo "$n|$(basename "$(dirname "$c")")|$r1|$r2"
done > $O/results.txt
wc -l $O/results.txt
