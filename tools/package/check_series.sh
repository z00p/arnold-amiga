#!/bin/bash
# Build each series step with the final headless host; checksum system cart + 3 homebrew carts.
A=$(cd "$(dirname "$0")/../.." && pwd); D=/opt/src/series-chk
rm -rf $D; mkdir -p $D
CARTS=("$A/arnold/roms/cpcplus/system.cpr" "$A/carts/homebrew/01 Sonic GX.cpr" "$A/carts/homebrew/22 Ghosts 'n Goblins Xifos.cpr" "$A/carts/homebrew/66 GX Pong.cpr" "$A/carts/homebrew/19 Puzzle Bobble cartridge v1.2.cpr")
for t in $A/build/series-chk/??.tar; do
  n=$(basename $t .tar); mkdir -p $D/$n; tar -xf $t -C $D
  cd $D/$n/src; mkdir -p headless amiga
  cp $A/build/series-chk/host/host_headless.c headless/
  for f in keyscript.c keyscript.h audio_conv.h; do cp $A/build/series-chk/host/$f amiga/; done
  SRCS="$(tr -d '\r' < objects.mak | sed -n '/^CPC_O=/,/^$/p' | tr -d '\' | sed 's/CPC_O=//;s/\.o/.c/g') headless/host_headless.c amiga/keyscript.c"
  err=$(gcc -O2 -std=gnu89 -fcommon -I. -DUNIX -DCPC_LSB_FIRST -DCPC_NODEBUGGER -w -o hd $SRCS -lm 2>&1 | head -3)
  line="$n:"
  if [ ! -x hd ]; then echo "$n BUILD FAILED: $err"; continue; fi
  for c in "${CARTS[@]}"; do cp "$c" /tmp/sc.cpr; s=$(./hd 600 /tmp/sc.ppm /tmp/sc.cpr 8 | grep -o 'checksum=.*' | cut -d= -f2); line="$line $s"; done
  echo "$line"
done
