# Speed of the working tree vs git HEAD, x86 headless, 3000 frames per cart. Run after headless_check.sh (uses its builds).
export PATH=/usr/local/bin:/usr/bin:/bin
R=${A:-$(cd "$(dirname "$0")/.." && pwd)}
for cart in "$R/arnold/roms/cpcplus/system.cpr" "$R/run/carts/PuzzleBobble.cpr" "$R/run/carts/GNGGX.cpr" "$R/run/carts/HeadOverHeels.cpr"; do
  cp "$cart" /tmp/c.cpr
  a=$( { /usr/bin/time -f %e /opt/src/hc-ref/src/hd 3000 /tmp/r.ppm /tmp/c.cpr 8 >/dev/null; } 2>&1 )
  b=$( { /usr/bin/time -f %e /opt/src/hc-new/src/hd 3000 /tmp/n.ppm /tmp/c.cpr 8 >/dev/null; } 2>&1 )
  python3 -c "import sys; a,b=float('$a'),float('$b'); print(f'%-20s old %.2fs  new %.2fs  saving %d%%' % ('$(basename "$cart")', a, b, round(100*(a-b)/a)))"
done
