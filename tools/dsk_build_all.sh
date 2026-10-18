# Builds the disk-test variants: hd (fixes on), hd_nofix (-DNO_DSK_FIX), hd_asan (AddressSanitizer)
B=$(cd "$(dirname "$0")/.." && pwd)/tools/dsk_build.sh
bash $B 2>&1 | grep -E 'error|built'
EXTRA=-DNO_DSK_FIX OUT=hd_nofix bash $B 2>&1 | grep -E 'error|built'
EXTRA='-fsanitize=address -fno-omit-frame-pointer -O1' OUT=hd_asan bash $B 2>&1 | grep -E 'error|built'
