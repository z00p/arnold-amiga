export PATH=/usr/local/bin:/usr/bin:/bin
A=${A:-$(cd "$(dirname "$0")/.." && pwd)}
cp $A/run/carts/PuzzleBobble.cpr /tmp/pf.cpr
cd /opt/src/hc-new/src && valgrind --tool=cachegrind --cache-sim=no --cachegrind-out-file=/tmp/cg.skip ./hd 300 /tmp/pf.ppm /tmp/pf.cpr 8 $A/tests/keys/generic.keys 3 >/dev/null 2>&1
cg_annotate --auto=no /tmp/cg.skip | grep -E "^ +[0-9,]+ +\(" | head -25
