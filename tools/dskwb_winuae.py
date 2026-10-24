"""Disk write-back on the Amiga build in WinUAE (22 Oct 2026).

    python -I tools/dskwb_winuae.py [--exe Arnold-dskwb] [--config Arnold-PiStorm.uae]

Take the WinUAE lock first (C:/AMD/amimsx/build/winuae.lock). One WinUAE boot runs
run/<exe> four times (KEYS= scripts, TURBO, QUIT ends each run), on generated
test disks copied to run/dsk-test/wb-amiga/ (never real game images):
  1. save_std: SAVE"AMIGA on a blank standard DSK -> written, .bak = original, standard
  2. resave:   the same disk again, RUN + SAVE -> the .old path (rename over an
               existing file on AmigaOS), .bak untouched, AMIGA.BAK in the directory
  3. save_ext: SAVE on a blank extended DSK -> stays extended
  4. wprot:    the file read-only (Windows R attribute = no W bit in WinUAE):
               not touched, no .bak, the log says write-protected
The Startup-Sequence and run/Arnold.log are restored afterwards."""
import argparse, os, shutil, stat, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import regress, make_dsk
from dsk_tests import keyscript, amsdos_dir

BASE, RUN = regress.BASE, regress.RUN
W = os.path.join(RUN, 'dsk-test', 'wb-amiga')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default='Arnold-dskwb')
    ap.add_argument('--config', default='Arnold-PiStorm.uae')
    ap.add_argument('--timeout', type=int, default=240)
    a = ap.parse_args()
    os.makedirs(W, exist_ok=True)
    gen = os.path.join(W, 'gen'); make_dsk.main(gen)
    for f in os.listdir(W):
        p = os.path.join(W, f)
        if os.path.isfile(p): os.chmod(p, stat.S_IWRITE); os.remove(p)
    for n, src in (('wbstd.dsk', 'blank_std.dsk'), ('wbext.dsk', 'blank_ext.dsk'), ('wprot.dsk', 'blank_std.dsk')):
        shutil.copyfile(os.path.join(gen, src), os.path.join(W, n))
    orig = {n: open(os.path.join(W, n), 'rb').read() for n in ('wbstd.dsk', 'wbext.dsk', 'wprot.dsk')}
    os.chmod(os.path.join(W, 'wprot.dsk'), stat.S_IREAD)

    save = [(4, 'KEY:F1'), (7, '10 PRINT "AMIGA SAVE OK"\n'), (12, 'SAVE"AMIGA\n'), (16, 'CAT\n'), (19, 'SNAP')]
    resave = [(4, 'KEY:F1'), (7, 'RUN"AMIGA\n'), (10, '20 REM SECOND\n'), (13, 'SAVE"AMIGA\n'), (17, 'CAT\n'), (20, 'SNAP')]
    runs = [('save_std', 'wbstd.dsk', save, 20), ('resave', 'wbstd.dsk', resave, 21),
            ('save_ext', 'wbext.dsk', save, 20), ('wprot', 'wprot.dsk', save, 20)]
    seq_backup = open(regress.SEQ, 'rb').read()
    log_path = os.path.join(RUN, 'Arnold.log')
    log_backup = open(log_path, 'rb').read() if os.path.exists(log_path) else None
    dones = []
    with open(os.path.join(RUN, 'Keys', 'batch'), 'w', encoding='latin-1', newline='\n') as bf:
        bf.write('; written by tools/dskwb_winuae.py\n')
        for rid, disk, steps, end in runs:
            ks = os.path.join(RUN, 'Keys', 'dskwb_%s.keys' % rid)
            open(ks, 'w', encoding='latin-1', newline='\n').write(keyscript(steps, end))
            sd = os.path.join(RUN, 'snaps', 'dskwb_' + rid)
            shutil.rmtree(sd, ignore_errors=True); os.makedirs(sd)
            dones.append(os.path.join(sd, 'done'))
            bf.write('Arnold:%s Arnold:dsk-test/wb-amiga/%s KEYS=Arnold:Keys/dskwb_%s.keys SNAPDIR=Arnold:snaps/dskwb_%s NOSOUND TURBO\n'
                     % (a.exe, disk, rid, rid))
        bf.write('Copy Arnold:Arnold.log Arnold:dsk-test/wb-amiga/last.log\n')
    results = []
    def check(name, ok, info=''):
        print('  %-36s %s %s' % (name, 'PASS' if ok else 'FAIL', info)); results.append(ok)
    try:
        regress.set_startup('Run >NIL: Execute Arnold:Keys/batch')
        regress.kill_uae()
        p = subprocess.Popen([regress.UAE, '-f', os.path.join(BASE, 'winuae', a.config)], cwd=os.path.dirname(regress.UAE))
        t0 = time.time()
        for (rid, *_), done in zip(runs, dones):
            while time.time() - t0 < a.timeout and not os.path.exists(done): time.sleep(0.5)
            print('  %s: %s after %.1fs' % (rid, 'done' if os.path.exists(done) else 'TIMEOUT', time.time() - t0), flush=True)
            if rid == 'save_std' and os.path.exists(done):      # snapshot the state between runs 1 and 2
                after1 = open(os.path.join(W, 'wbstd.dsk'), 'rb').read()
        last = os.path.join(W, 'last.log')
        while time.time() - t0 < a.timeout and not os.path.exists(last): time.sleep(0.5)
        time.sleep(2)
    finally:
        regress.kill_uae()
        open(regress.SEQ, 'wb').write(seq_backup)
        if log_backup is not None: open(log_path, 'wb').write(log_backup)
    D = lambda n: os.path.join(W, n)
    s = open(D('wbstd.dsk'), 'rb').read()
    f = amsdos_dir(D('wbstd.dsk'))
    check('std: written, still standard', s != orig['wbstd.dsk'] and s[:8] == b'MV - CPC' and len(s) == len(orig['wbstd.dsk']))
    check('std: .bak = original', open(D('wbstd.dsk.bak'), 'rb').read() == orig['wbstd.dsk'])
    check('std: AMIGA.BAS saved, resaved (.old path)', 'AMIGA.BAS' in f and 'AMIGA.BAK' in f and b'second' in f['AMIGA.BAS'].lower(), str(sorted(f)))
    check('std: no .tmp / .old left', not os.path.exists(D('wbstd.dsk.tmp')) and not os.path.exists(D('wbstd.dsk.old')))
    e = open(D('wbext.dsk'), 'rb').read()
    check('ext: written, still extended', e[:8] == b'EXTENDED' and 'AMIGA.BAS' in amsdos_dir(D('wbext.dsk')) and
          open(D('wbext.dsk.bak'), 'rb').read() == orig['wbext.dsk'])
    w = open(D('wprot.dsk'), 'rb').read()
    log = open(D('last.log'), 'r', encoding='latin-1').read() if os.path.exists(D('last.log')) else ''
    check('read-only file: not touched, no .bak', w == orig['wprot.dsk'] and not os.path.exists(D('wprot.dsk.bak')))
    check('read-only file: logged', 'write-protected' in log, [l for l in log.splitlines() if 'drive A (' in l])
    os.chmod(D('wprot.dsk'), stat.S_IWRITE)
    print('SUMMARY: %d/%d PASS' % (sum(results), len(results)))
    return 0 if all(results) else 1

if __name__ == '__main__':
    sys.exit(main())
