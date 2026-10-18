# Frame skip must not change a drawn frame: the frame-skip hook with a pseudo-random pattern
# (headless argv[6] = -1, about 3 in 4 skipped) against drawing all, same emulated frame, every cart. Uses /opt/src/hc-new (headless_check.sh).
export PATH=/usr/local/bin:/usr/bin:/bin
HD=${HD:-/opt/src/hc-new/src/hd}
A=${A:-$(cd "$(dirname "$0")/.." && pwd)}
K=$A/tests/keys/generic.keys
same=0; diff=0
# Cart set: John's local test carts (run/carts, not in git) when present, else the
# repo's homebrew set (carts/homebrew). SET=homebrew forces the repo set.
if [ -z "$SET" ] && [ -d "$A/run/carts" ]; then
  CARTS=("$A/arnold/roms/cpcplus/system.cpr" "$A"/run/carts/*.cpr "$A"/run/carts/top80/*.cpr)
else
  CARTS=("$A/arnold/roms/cpcplus/system.cpr" "$A"/carts/homebrew/*.cpr)
fi
for cart in "${CARTS[@]}"; do
  cp "$cart" /tmp/k.cpr
  for n in 350 750 1500; do
    a=$($HD $((2*n)) /tmp/ka.ppm /tmp/k.cpr 8 $K 0 | grep -o 'checksum=.*')
    b=$($HD $((2*n)) /tmp/kb.ppm /tmp/k.cpr 8 $K -1 | grep -o 'checksum=.*')
    if [ "$a" = "$b" ]; then same=$((same+1)); else diff=$((diff+1)); echo "DIFF $(basename "$cart") at $((2*n)): $a vs $b"; fi
  done
done
echo "skip check: $same same, $diff different"
