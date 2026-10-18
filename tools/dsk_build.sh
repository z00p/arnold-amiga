# Build the headless host for the disk tests into its own directory
# (wsl -d Debian -u root -- bash <repo>/tools/dsk_build.sh).
# EXTRA="-DNO_DSK_FIX" builds with the disk-image fixes switched off.
export PATH=/usr/local/bin:/usr/bin:/bin
A=${A:-$(cd "$(dirname "$0")/.." && pwd)}
D=${D:-/opt/src/dsk-build}
mkdir -p $D
rsync -a --delete --exclude 'obj-*' --exclude 'Arnold*' --exclude 'AmiCPC*' $A/arnold/src/ $D/src/
cd $D/src
SRCS="$(tr -d '\r' < objects.mak | sed -n '/^CPC_O=/,/^$/p' | tr -d '\' | sed 's/CPC_O=//;s/\.o/.c/g') headless/host_headless.c amiga/keyscript.c"
gcc -O2 -g $EXTRA -std=gnu89 -fcommon -I. -DUNIX -DCPC_LSB_FIRST -DCPC_NODEBUGGER -w -o $D/${OUT:-hd} $SRCS -lm && echo "built $D/${OUT:-hd}"
