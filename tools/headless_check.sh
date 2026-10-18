# Pixel-exact check of core changes (run in WSL: wsl -d Debian -u root -- bash -s < tools/headless_check.sh)
# Builds the x86 headless host twice - core from the working tree, and core from REF
# (default HEAD; set REF=<commit> in the environment) - both with the working tree's
# headless host and key-script code, then runs every cartridge with the generic play
# script and compares frame checksums at several points. NEWFLAGS="-DNO_B ..." builds
# the working tree with speed-ups switched off (diagnosis).
export PATH=/usr/local/bin:/usr/bin:/bin
REF=${REF:-HEAD}
A=${A:-$(cd "$(dirname "$0")/.." && pwd)}
build() {  # dir
  cd $1/src
  SRCS="$(tr -d '\r' < objects.mak | sed -n '/^CPC_O=/,/^$/p' | tr -d '\\' | sed 's/CPC_O=//;s/\.o/.c/g') headless/host_headless.c amiga/keyscript.c"
  gcc -O2 $EXTRA -std=gnu89 -fcommon -I. -DUNIX -DCPC_LSB_FIRST -DCPC_NODEBUGGER -w -o hd $SRCS -lm 2>&1 | head -3
  cd - >/dev/null
}
rm -rf /opt/src/hc-new /opt/src/hc-ref && mkdir -p /opt/src/hc-new /opt/src/hc-ref
rsync -a --exclude 'obj-*' --exclude 'Arnold*' --exclude 'AmiCPC*' $A/arnold/src/ /opt/src/hc-new/src/
git -c safe.directory=$A -C $A archive $REF arnold/src | tar -x --strip-components=1 -C /opt/src/hc-ref
mkdir -p /opt/src/hc-ref/src/amiga
cp $A/arnold/src/headless/host_headless.c /opt/src/hc-ref/src/headless/
cp $A/arnold/src/amiga/keyscript.c $A/arnold/src/amiga/keyscript.h $A/arnold/src/amiga/audio_conv.h /opt/src/hc-ref/src/amiga/
EXTRA="$NEWFLAGS" build /opt/src/hc-new; EXTRA="" build /opt/src/hc-ref
KEYS=$A/tests/keys/generic.keys
same=0; diff=0
# Cart set: John's local test carts (run/carts, not in git) when present, else the
# repo's homebrew set (carts/homebrew). SET=homebrew forces the repo set.
if [ -z "$SET" ] && [ -d "$A/run/carts" ]; then
  CARTS=("$A/arnold/roms/cpcplus/system.cpr" "$A"/run/carts/*.cpr "$A"/run/carts/top80/*.cpr)
else
  CARTS=("$A/arnold/roms/cpcplus/system.cpr" "$A"/carts/homebrew/*.cpr)
fi
for cart in "${CARTS[@]}"; do
  cp "$cart" /tmp/c.cpr
  for n in 700 1500 3000; do
    a=$(/opt/src/hc-ref/src/hd $n /tmp/r.ppm /tmp/c.cpr 8 $KEYS | grep -o 'checksum=.*')
    b=$(/opt/src/hc-new/src/hd $n /tmp/n.ppm /tmp/c.cpr 8 $KEYS | grep -o 'checksum=.*')
    if [ "$a" = "$b" ]; then same=$((same+1)); else diff=$((diff+1)); echo "DIFF $(basename "$cart") frame $n: $a vs $b"; fi
  done
done
echo "headless check vs $REF: $same same, $diff different"
