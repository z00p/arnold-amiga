# x86 instructions per emulated frame: drawing every frame vs the frame-skip hook skipping ~3 in 4
# (headless argv[6] = -1). Uses /opt/src/hc-new (headless_check.sh).
export PATH=/usr/local/bin:/usr/bin:/bin
A=${A:-$(cd "$(dirname "$0")/.." && pwd)}
ir() { valgrind --tool=cachegrind --cache-sim=no --cachegrind-out-file=/dev/null "$@" 2>&1 | grep "I *refs" | sed 's/.*refs: *//' | tr -dc '0-9'; }
for c in "$A/run/carts/HeadOverHeels.cpr $A/tests/keys/hoh.keys" "$A/arnold/roms/cpcplus/system.cpr $A/tests/keys/burnin.keys" "$A/run/carts/PuzzleBobble.cpr $A/tests/keys/generic.keys" "$A/run/carts/GNGGX.cpr $A/tests/keys/generic.keys"; do
  set -- $c; cp $1 /tmp/sk.cpr
  n=$(/opt/src/hc-new/src/hd 800 /tmp/s.ppm /tmp/sk.cpr 8 $2 -1 | grep -o 'frames=[0-9]*' | tr -dc 0-9)
  a=$(ir /opt/src/hc-new/src/hd 800 /tmp/s.ppm /tmp/sk.cpr 8 $2 0)
  b=$(ir /opt/src/hc-new/src/hd 800 /tmp/s.ppm /tmp/sk.cpr 8 $2 -1)
  python3 -c "a,b,n=$a/800,$b/800,$n; d=800-n; s=(800*b-n*a)/d; print('%-20s drawn frame %.2fM  skipped frame %.2fM (%2.0f%% of drawn; %d of 800 drawn)' % ('$(basename $1)', a/1e6, s/1e6, 100*s/a, n))"
done
