#!/bin/bash
# dskwb_build.sh: the 5 Amiga variants with the existing PGO profile (tools/perf50/pgo),
# as tools/perf50/build_variants_pgo.sh, but built in /opt/src/dskwb-pgo and put in
# build/dskwrite/ (the perf50 release builds stay as they are). Functions changed since
# the profile was recorded get no profile data (coverage mismatch: warning, not error).
export PATH=/opt/amiga/bin:/usr/local/bin:/usr/bin:/bin
W=$(cd "$(dirname "$0")/.." && pwd); D=/opt/src/dskwb-pgo; OUT=$W/build/dskwrite
F="-fno-function-cse -fno-unroll-loops -fno-peel-loops"
mkdir -p $OUT
for v in ${VARIANTS:-"Arnold:68040:-O2" "Arnold-040-Max:68040:-O3" "Arnold-060:68060:-O2" "Arnold-060-Max:68060:-O3" "Arnold-030:68030:-O2"}; do
  IFS=: read n c o <<< "$v"
  cd /; rm -rf $D; mkdir -p $D
  rsync -a --exclude 'obj-*' --exclude "Arnold*" $W/arnold/src/ $D/src/
  mkdir -p $D/prof && cp -r $W/tools/perf50/pgo/obj-pgo $D/prof/
  cd $D/src/amiga
  make -j6 TARGET=$n CPU=$c OPT="$o $F -fprofile-use=$D/prof -fprofile-correction -Wno-error=coverage-mismatch" OBJDIR=obj-pgo > $D/use.log 2>&1
  grep -E "error|coverage" $D/use.log | grep -v "^m68k-amigaos-gcc" | head -8
  cp $n $OUT/$n && ls -la $OUT/$n | awk '{print $5, $9}'
done
