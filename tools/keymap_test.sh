#!/bin/bash
# keymap_test.sh : unit checks for the key map logic (run in WSL: bash tools/keymap_test.sh)
A=${A:-$(cd "$(dirname "$0")/.." && pwd)/build/keymap}
D=/opt/src/arnold-keymap-test; rm -rf $D; mkdir -p $D
cp -r $A/arnold/src $D/src
cd $D/src && gcc -std=gnu89 -fcommon -I. -DUNIX -DCPC_LSB_FIRST -DCPC_NODEBUGGER -w -o $D/kt $A/tools/keymap_test.c amiga/keymaplogic.c amiga/keyscript.c -lm 2>&1 | head -20
$D/kt
