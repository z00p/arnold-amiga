# Function-level x86 instruction profile (valgrind cachegrind) of the headless build
# made by headless_check.sh (/opt/src/hc-new). Writes build/profile/<cart>.txt.
export PATH=/usr/local/bin:/usr/bin:/bin
A=${A:-$(cd "$(dirname "$0")/.." && pwd)}
prof() { # cart keys frames name
  cp "$1" /tmp/pf.cpr
  ( cd /opt/src/hc-new/src && valgrind --tool=cachegrind --cache-sim=no --cachegrind-out-file=/tmp/cg.$4 ./hd $3 /tmp/pf.ppm /tmp/pf.cpr 8 $2 >/dev/null 2>&1 )
  cg_annotate --auto=no /tmp/cg.$4 2>/dev/null | head -60 > $A/build/profile/$4.txt
}
prof $A/run/carts/HeadOverHeels.cpr $A/tests/keys/hoh.keys 3000 hoh
prof $A/arnold/roms/cpcplus/system.cpr $A/tests/keys/burnin.keys 1000 burnin
prof $A/run/carts/PuzzleBobble.cpr $A/tests/keys/generic.keys 700 pbobble
echo profiled
