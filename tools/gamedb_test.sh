#!/bin/bash
# gamedb_test.sh : unit checks for Games.db (run in WSL: bash tools/gamedb_test.sh)
# gamedb.c + the real DskWrite_SaveFile, built with the address and undefined-behaviour sanitizers.
export PATH=/usr/local/bin:/usr/bin:/bin
A=${A:-$(cd "$(dirname "$0")/.." && pwd)/build/gamedb}
D=/opt/src/arnold-gamedb-test; rm -rf $D; mkdir -p $D
cp -r $A/arnold/src $D/src
cd $D/src && gcc -g -std=gnu89 -fcommon -fsanitize=address,undefined -fno-sanitize-recover=undefined -I. -DUNIX -DCPC_LSB_FIRST -DCPC_NODEBUGGER -w \
  -o $D/gt $A/tools/gamedb_test.c amiga/gamedb.c cpc/diskimage/dskwrite.c 2>&1 | head -20
$D/gt $A/tools/package/Games.db
