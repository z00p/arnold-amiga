# Deterministic speed measure: x86 instructions executed (valgrind cachegrind, no cache sim)
# for 600 frames per cart, working tree vs REF (uses the builds headless_check.sh made; run it first).
export PATH=/usr/local/bin:/usr/bin:/bin
A=${A:-$(cd "$(dirname "$0")/.." && pwd)}
K=$A/tests/keys/generic.keys
ir() { valgrind --tool=cachegrind --cache-sim=no --cachegrind-out-file=/dev/null "$@" 2>&1 | grep "I *refs" | sed 's/.*refs: *//' | tr -dc '0-9'; }
if [ -z "$SET" ] && [ -d "$A/run/carts" ]; then
  CARTS=("$A/arnold/roms/cpcplus/system.cpr" "$A/run/carts/PuzzleBobble.cpr" "$A/run/carts/GNGGX.cpr" "$A/run/carts/top80/004.cpr" "$A/run/carts/top80/001.cpr")
else
  H="$A/carts/homebrew"
  CARTS=("$A/arnold/roms/cpcplus/system.cpr" "$H/19 Puzzle Bobble cartridge v1.2.cpr" "$H/22 Ghosts 'n Goblins Xifos.cpr" "$H/01 Sonic GX.cpr" "$H/12 Bomb Jack GX.cpr")
fi
for cart in "${CARTS[@]}"; do
  cp "$cart" /tmp/ic.cpr
  a=$(ir /opt/src/hc-ref/src/hd 600 /tmp/ir.ppm /tmp/ic.cpr 8 $K)
  b=$(ir /opt/src/hc-new/src/hd 600 /tmp/in.ppm /tmp/ic.cpr 8 $K)
  python3 -c "a,b=$a,$b; print('%-20s ref %6.0fM  new %6.0fM  saving %4.1f%%' % ('$(basename "$cart" | tr -d "'")', a/1e6, b/1e6, 100*(a-b)/a))"
done
