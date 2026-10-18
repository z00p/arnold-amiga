# wsl -d Debian -u root -- bash <repo>/tools/dsk_one.sh NAME END STEPSFILE DISKA [DISKB]
cd $(cd "$(dirname "$0")/.." && pwd)/run/dsk-test && python3 ../../tools/dsk_tests.py one ${HD:-/opt/src/dsk-build/hd} . "$1" "$2" "$4" "$5" < "$3"
