# STATS_E build of the working tree; per-frame CRTC stepping counters (stderr) for a cart.
export PATH=/usr/local/bin:/usr/bin:/bin
A=${A:-$(cd "$(dirname "$0")/.." && pwd)}
rm -rf /opt/src/hs && mkdir -p /opt/src/hs && rsync -a --exclude 'obj-*' --exclude 'Arnold*' --exclude 'AmiCPC*' $A/arnold/src/ /opt/src/hs/src/
cd /opt/src/hs/src
SRCS="$(tr -d '\r' < objects.mak | sed -n '/^CPC_O=/,/^$/p' | tr -d '\' | sed 's/CPC_O=//;s/\.o/.c/g') headless/host_headless.c amiga/keyscript.c"
gcc -O2 -DSTATS_E $EXTRA -std=gnu89 -fcommon -I. -DUNIX -DCPC_LSB_FIRST -DCPC_NODEBUGGER -w -o hd $SRCS -lm 2>&1 | head -3
for skip in 0 3; do
 for c in "HeadOverHeels.cpr hoh.keys" "PuzzleBobble.cpr generic.keys"; do set -- $c
  cp $A/run/carts/$1 /tmp/st.cpr; echo "$1 skip $skip:"; ./hd 600 /tmp/st.ppm /tmp/st.cpr 8 $A/tests/keys/$2 $skip 2>&1 >/dev/null
 done
done
