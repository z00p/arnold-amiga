# Build the four Amiga speed-test variants into run/ (WSL root shell).
# Build the speed-test variants of Arnold (Arnold core) and copy them to run/.
export PATH=/opt/amiga/bin:/usr/local/bin:/usr/bin:/bin
rm -rf /opt/src/arnold-build && mkdir -p /opt/src/arnold-build && rsync -a --exclude 'obj-*' --exclude "Arnold*" ${A:-$(cd "$(dirname "$0")/.." && pwd)}/arnold/src/ /opt/src/arnold-build/src/
cd /opt/src/arnold-build/src/amiga
b() {  # name cpu opt
  make -j6 TARGET=$1 CPU=$2 OPT="$3" OBJDIR=obj-$1 2>&1 | grep -E "error|Error" | head -5
  ls -la $1 | awk '{print $5, $9}' && cp $1 ${A:-$(cd "$(dirname "$0")/.." && pwd)}/run/$1
}
b Arnold         68040 "-O2"
b Arnold-040-Max 68040 "-O3"
b Arnold-060     68060 "-O2"
b Arnold-060-Max 68060 "-O3"
