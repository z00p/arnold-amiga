#!/usr/bin/env python3
"""Boots real disk images headlessly (run in WSL):
    python3 tools/dsk_real.py [HD] [DISKDIR] [OUTDIR]
DISKDIR defaults to run/disks-local (personal images: gitignored, never commit).
For each image: format summary, the core's sectors against our own reading of
the file (tools/dsk_tests.py), then CAT and the run command (RUN"<file> from the
AMSDOS directory, or |CPM for a disk with no directory) with snaps."""
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dsk_tests as T

def summary(path):
    d = open(path, 'rb').read()
    ext = d[:8] == b'EXTENDED'
    tr = T.read_image(path)
    ids = {}
    odd = []
    for (t, s), secs in sorted(tr.items()):
        if not secs: odd.append('T%d.%d unformatted' % (t, s)); continue
        key = tuple(sorted(set(q[2] & 0xF0 for q in secs)))
        ids[key] = ids.get(key, 0) + 1
        ns = [q[3] for q in secs]
        if len(secs) != 9 or set(ns) != {2}:
            odd.append('T%d.%d %dx[%s]' % (t, s, len(secs), ','.join('%02X/N%d' % (q[2], q[3]) for q in secs[:6])))
        for q in secs:
            if q[4] or q[5]: odd.append('T%d.%d R%02X ST1=%02X ST2=%02X len=%d' % (t, s, q[2], q[4], q[5], len(q[6])))
    return '%s %d tracks x %d sides, %d bytes (len %% 256 = %d); sector ID groups %s; %d unusual: %s' % (
        'EDSK' if ext else 'DSK', d[0x30], d[0x31], len(d), len(d) % 256,
        {'%s' % ','.join('%02X' % k for k in key): n for key, n in ids.items()}, len(odd), '; '.join(odd[:8]))

def run_name(path):
    tr = T.read_image(path)
    if not any(q[2] in (0xC1, 0x41) for q in tr.get((0, 0), [])) and not any(q[2] == 0x41 for q in tr.get((2, 0), [])):
        return None, ['(not an AMSDOS DATA/SYSTEM format)']
    try:
        files = T.amsdos_dir(path)
    except Exception:
        return None, []
    names = sorted(n for n in files if all(32 < ord(c) < 127 for c in n))
    for pref in ('DISC.BAS', 'DISK.BAS', 'DISC.', 'DISK.', 'DISC.BIN', 'DISK.BIN', 'RUN.BAS', 'MENU.BAS', 'LOADER.BAS'):
        if pref in names: return pref.rstrip('.'), names
    bas = [n for n in names if n.endswith('.BAS')]
    if len(bas) == 1: return bas[0], names
    return (bas or names or [None])[0], names

def main():
    hd = sys.argv[1] if len(sys.argv) > 1 else '/opt/src/dsk-build/hd'
    src = sys.argv[2] if len(sys.argv) > 2 else os.path.join(T.A, 'run', 'disks-local')
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.join(T.A, 'run', 'disks-local', 'out')
    os.makedirs(out, exist_ok=True)
    override = {}
    if os.path.exists(os.path.join(src, 'run.txt')):          # <image> <command>  (optional overrides)
        for line in open(os.path.join(src, 'run.txt')):
            if line.strip() and not line.startswith('#'):
                a, _, b = line.rstrip('\n').partition('\t'); override[a] = b
    for f in sorted(os.listdir(src)):
        if not f.lower().endswith('.dsk'): continue
        p = os.path.join(src, f)
        tag = re.sub(r'[^A-Za-z0-9]+', '_', f[:-4])[:40]
        print('#### %s\n  %s' % (f, summary(p)))
        T.dump_check(hd, out, tag, p)
        name, names = run_name(p)
        print('  directory: %s' % ' '.join(names[:16]))
        if names[:1] == ['(not an AMSDOS DATA/SYSTEM format)'] and f not in override:
            print('  skipped: needs a DOS other than AMSDOS'); continue
        cmd = override.get(f) or ('RUN"%s' % name if name else '|CPM')
        print('  run: %s' % cmd)
        steps = [(4, 'KEY:F1'), (6.5, 'CAT\n'), (11, 'SNAP'), (11.5, cmd + '\n'), (20, 'SNAP'), (30, 'SNAP'), (45, 'SNAP'), (60, 'SNAP')]
        rc, log, snaps = T.run(hd, out, tag, steps, 61, diska=p)
        ref = T.run_cpcec(out, tag, steps, 61, p) if os.path.isdir(T.CPCEC) and not os.environ.get('NOCPCEC') else []
        if snaps and len(ref) == len(snaps):      # Arnold left, CPCEC right, one row a snap
            T.montage([x for pair in zip(snaps, ref) for x in pair], os.path.join(out, 'M_' + tag + '.png'), cols=2)
        elif snaps: T.montage(snaps, os.path.join(out, 'M_' + tag + '.png'))

if __name__ == '__main__':
    main()
