#!/usr/bin/env python3
"""List the blocks of TZX/CDT tape images: id, length, timings (python3 tools/tzx_info.py f.cdt ...)."""
import struct, sys

def blocks(d):
    if d[:8] != b'ZXTape!\x1a':
        raise ValueError('not a TZX/CDT file')
    p = 10
    while p < len(d):
        i = d[p]; q = p + 1
        if i == 0x10: n = 4 + struct.unpack_from('<H', d, q + 2)[0]; info = 'std pause=%d len=%d' % (struct.unpack_from('<H', d, q)[0], n - 4)
        elif i == 0x11:
            pil, s1, s2, z, o, pl, used, pause = struct.unpack_from('<HHHHHHBH', d, q)
            ln = d[q + 15] | d[q + 16] << 8 | d[q + 17] << 16; n = 18 + ln
            info = 'turbo pilot=%d x%d sync=%d/%d 0=%d 1=%d pause=%d len=%d' % (pil, pl, s1, s2, z, o, pause, ln)
        elif i == 0x12: n = 4; info = 'tone %d x%d' % struct.unpack_from('<HH', d, q)
        elif i == 0x13: n = 1 + 2 * d[q]; info = 'pulses x%d' % d[q]
        elif i == 0x14:
            z, o, used, pause = struct.unpack_from('<HHBH', d, q); ln = d[q + 7] | d[q + 8] << 8 | d[q + 9] << 16; n = 10 + ln
            info = 'pure 0=%d 1=%d pause=%d len=%d' % (z, o, pause, ln)
        elif i == 0x15: ln = d[q + 5] | d[q + 6] << 8 | d[q + 7] << 16; n = 8 + ln; info = 'direct len=%d' % ln
        elif i == 0x18 or i == 0x19: n = 4 + struct.unpack_from('<I', d, q)[0]; info = 'csw/generalised'
        elif i == 0x20: n = 2; info = 'pause %d' % struct.unpack_from('<H', d, q)[0]
        elif i == 0x21: n = 1 + d[q]; info = 'group ' + d[q + 1:q + n].decode('latin1')
        elif i == 0x22: n = 0; info = 'group end'
        elif i == 0x23: n = 2; info = 'jump'
        elif i == 0x24: n = 2; info = 'loop %d' % struct.unpack_from('<H', d, q)[0]
        elif i == 0x25: n = 0; info = 'loop end'
        elif i == 0x2a: n = 4; info = 'stop if 48K'
        elif i == 0x2b: n = 5; info = 'signal level'
        elif i == 0x30: n = 1 + d[q]; info = 'text ' + d[q + 1:q + n].decode('latin1')
        elif i == 0x31: n = 2 + d[q + 1]; info = 'message'
        elif i == 0x32: n = 2 + struct.unpack_from('<H', d, q)[0]; info = 'archive info'
        elif i == 0x33: n = 1 + 3 * d[q]; info = 'hw type'
        elif i == 0x35: n = 20 + struct.unpack_from('<I', d, q + 16)[0]; info = 'custom'
        elif i == 0x5a: n = 9; info = 'glue'
        else: n = 4 + struct.unpack_from('<I', d, q)[0]; info = 'UNKNOWN'
        yield i, info
        p = q + n

if __name__ == '__main__':
    for f in sys.argv[1:]:
        try:
            b = list(blocks(open(f, 'rb').read()))
        except Exception as e:
            print(f, 'ERROR', e); continue
        ids = sorted(set('%02X' % i for i, _ in b))
        print('%s: %d blocks, ids %s' % (f, len(b), ' '.join(ids)))
        if '-v' in sys.argv[0:1] or len(sys.argv) == 2:
            for i, info in b: print('  %02X %s' % (i, info))
