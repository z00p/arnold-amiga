#!/usr/bin/env python3
"""Disk write-back tests (22 Oct 2026) for the headless Arnold host (run in WSL):
    python3 tools/dskwb_tests.py [HD] [OUTDIR]
HD defaults to /opt/src/dskwb-build/hd (D=/opt/src/dskwb-build tools/dsk_build.sh).
The host runs with DISKWB=1: at the end it writes each changed drive back into
its own file through cpc/diskimage/dskwrite.c, the code the Amiga host uses
(same format, patched sectors, <name>.tmp + rename, <name>.bak the first time).
Test disks are generated (tools/make_dsk.py) into OUTDIR/wb (run/dsk-test/wb);
the write-protect case uses an immutable file on the Linux side.
CPCEC (a private copy of /opt/src/cpcec-dsk set to read-write) runs the SAVE
script on the same blank disk for a byte comparison."""
import os, shutil, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_dsk, dsk_tests
from dsk_tests import run, read_image, amsdos_dir, header_ok, A

HD = sys.argv[1] if len(sys.argv) > 1 else '/opt/src/dskwb-build/hd'
OUT = os.path.join(sys.argv[2] if len(sys.argv) > 2 else os.path.join(A, 'run', 'dsk-test'), 'wb')
ROMS = os.path.join(A, 'arnold', 'roms')
results = []

def check(name, ok, info=''):
    print('  %-34s %s %s' % (name, 'PASS' if ok else 'FAIL', info))
    results.append((name, ok))

def fresh(src, dst):
    for ext in ('', '.bak', '.tmp', '.old'):
        if os.path.exists(dst + ext): os.remove(dst + ext)
    shutil.copyfile(src, dst)
    return dst

def wb_run(name, steps, end, **kw):
    os.environ['DISKWB'] = '1'
    try:
        rc, log, snaps = run(HD, OUT, name, steps, end, **kw)
    finally:
        os.environ.pop('DISKWB', None)
    return log

def sectors(path):
    """{(track, side, R): data} of an image (read the way the core reads it)"""
    return {(t, s, q[2]): q[6] for (t, s), secs in read_image(path).items() for q in secs}

def diff_ranges(a, b):
    """byte ranges where two equal-length files differ"""
    out, i = [], 0
    while i < len(a):
        if a[i] != b[i]:
            j = i
            while j < len(a) and a[j] != b[j]: j += 1
            out.append((i, j)); i = j
        else: i += 1
    return out

# -------------------------------------------------------------- Z80 -------
def prog_hiscore():
    """HISCORE.BIN at &4000: the AMSDOS BIOS (KL FIND COMMAND &84 read sector,
    &85 write sector) reads a sector of drive B, puts a 'high score' in it and
    writes it back: track 20 &C5, then track 39 &C2 (beside the weak sector)."""
    a = make_dsk.Asm(0x4000)
    def bios(cmd, track, sector, label):
        a.op_w(0x21, cmd); a.op_w(0xCD, 0xBCD4); a.op_w(0xD2, 'fail')   # ld hl,name; call KL FIND COMMAND; jp nc,fail
        a.op_w(0x22, 'far'); a.db(0x79); a.op_w(0x32, 'farrom')          # ld (far),hl; ld a,c; ld (far+2),a
        a.op_w(0x21, 0x5000); a.op_w(0x11, (track << 8) | 1); a.db(0x0E, sector)   # hl=buf, d=track e=drive B, c=sector
        a.db(0xDF); a.dw('far')                                          # rst &18: far call
    for track, sector, text in ((20, 0xC5, 'HISCORE 12345 JOHN'), (39, 0xC2, 'HISCORE 2 KEVIN')):
        bios('rd', track, sector, 'r')
        a.op_w(0x21, 'h%d' % track); a.op_w(0x11, 0x5000); a.op_w(0x01, len(text)); a.db(0xED, 0xB0)   # ldir the text in
        bios('wr', track, sector, 'w')
    make_dsk.emit_print(a, 'ok'); a.db(0xC9)
    a.L('fail'); make_dsk.emit_print(a, 'bad'); a.db(0xC9)
    a.L('rd'); a.db(0x84); a.L('wr'); a.db(0x85)
    a.L('far'); a.db(0, 0); a.L('farrom'); a.db(0)
    a.L('h20'); a.db('HISCORE 12345 JOHN'); a.L('h39'); a.db('HISCORE 2 KEVIN')
    a.L('ok'); a.db('HISCORE SAVED', 13, 10, 0); a.L('bad'); a.db('NO BIOS', 13, 10, 0)
    return a.build()

def main():
    os.makedirs(OUT, exist_ok=True)
    gen = os.path.join(OUT, 'gen')
    make_dsk.main(gen)
    G = lambda n: os.path.join(gen, n)
    D = lambda n: os.path.join(OUT, n)
    fs = make_dsk.Amsdos('DATA')
    fs.add('FMTALL.BIN', make_dsk.prog_fdctest([('fmt', t, 0, t, 0, 0xC1, 2, 9) for t in range(40)], drive=1, filler=0xE5), (2, 0x4000, 0x4000))
    make_dsk.write_dsk(D('tool_fmt.dsk'), make_dsk.fs_tracks(fs, make_dsk.ILV))
    fs = make_dsk.Amsdos('DATA')
    fs.add('HISCORE.BIN', prog_hiscore(), (2, 0x4000, 0x4000))
    make_dsk.write_dsk(D('tool_hs.dsk'), make_dsk.fs_tracks(fs, make_dsk.ILV))

    # 1. SAVE from BASIC on a blank standard and extended disk, second run reloads
    # typing takes 6 frames a character: keep each line clear of the next
    save = [(4, 'KEY:F1'), (7, '10 PRINT "SAVED PROGRAM OK"\n'), (12, 'SAVE"MYPROG\n'), (16, 'SAVE"TEXT",A\n'),
            (21, 'FOR I=0 TO 255:POKE &4000+I,I:NEXT\n'), (27, 'SAVE"DATA",B,&4000,256\n'), (33, 'CAT\n'), (36, 'SNAP')]
    SAVE_END = 37
    for n in ('blank_std', 'blank_ext'):
        d = fresh(G(n + '.dsk'), D('save_' + n + '.dsk'))
        orig = open(d, 'rb').read()
        log = wb_run('save_' + n, save, SAVE_END, diska=d)
        new = open(d, 'rb').read()
        check(n + ' written back', 'written' in log and new != orig)
        check(n + ' same format', new[:8] == orig[:8] and len(new) == len(orig), '%s %d bytes' % (new[:8], len(new)))
        hdr_same = all(new[o:o + 0x100] == orig[o:o + 0x100] for o in [0] + [0x100 + i * ((len(orig) - 0x100) // 40) for i in range(40)]) \
            if n == 'blank_std' else new[:0x100] == orig[:0x100]
        check(n + ' headers untouched', hdr_same)
        check(n + ' .bak = original', os.path.exists(d + '.bak') and open(d + '.bak', 'rb').read() == orig)
        check(n + ' no .tmp/.old left', not os.path.exists(d + '.tmp') and not os.path.exists(d + '.old'))
        files = amsdos_dir(d)
        ok = 'MYPROG.BAS' in files and header_ok(files['MYPROG.BAS']) and 'TEXT.' in files and \
            b'saved program ok' in files['TEXT.'].lower() and 'DATA.BIN' in files and files['DATA.BIN'][128:128 + 256] == bytes(range(256))
        check(n + ' files in the image', ok, str(sorted(files)))
        # reload (a second session): run it, save again; .bak stays the original
        log = wb_run('save2_' + n, [(4, 'KEY:F1'), (7, 'RUN"MYPROG\n'), (10, '20 PRINT "SECOND"\n'), (14, 'SAVE"MYPROG\n'),
                     (18, 'CAT\n'), (21, 'SNAP')], 22, diska=d)
        files = amsdos_dir(d)
        check(n + ' reload + resave', 'MYPROG.BAK' in files and b'second' in files.get('MYPROG.BAS', b'').lower(), str(sorted(files)))
        check(n + ' .bak made once', open(d + '.bak', 'rb').read() == orig)

    # 1b. the same SAVE script on CPCEC (read-write copy): byte compare
    cp = '/opt/src/cpcec-wb'
    if os.path.isdir(dsk_tests.CPCEC):
        if not os.path.isdir(cp): shutil.copytree(dsk_tests.CPCEC, cp, symlinks=True)
        open(os.path.join(cp, '.cpcecrc'), 'w').write('unit 0\n')
        dsk_tests.CPCEC = cp
        c = fresh(G('blank_std.dsk'), D('cpcec_save.dsk'))
        dsk_tests.run_cpcec(OUT, 'cpcec_save', save, SAVE_END, c)
        c_img = open(c, 'rb').read()
        # Arnold's image after the same one session
        d = fresh(G('blank_std.dsk'), D('save_cmp.dsk'))
        wb_run('save_cmp', save, SAVE_END, diska=d)
        a_img = open(d, 'rb').read()
        rng = diff_ranges(a_img, c_img) if len(a_img) == len(c_img) else ['length %d vs %d' % (len(a_img), len(c_img))]
        # the only difference allowed: bytes AMSDOS writes from memory it never set
        # (file tails, header padding), which a fresh Arnold holds as 00 and CPCEC as FF
        ram = rng and not isinstance(rng[0], str) and all(set(a_img[x0:x1]) == {0} and set(c_img[x0:x1]) == {0xFF} for x0, x1 in rng)
        check('SAVE image == CPCEC image', not rng or ram, 'byte for byte, %d bytes' % len(a_img) if not rng else
              '%d bytes in %d runs differ, all uninitialised RAM (00 vs FF)' % (sum(x1 - x0 for x0, x1 in rng), len(rng)) if ram else 'differs at %s' % rng[:6])
        if rng and not ram:
            sa, sc = sectors(d), sectors(c)
            check('  (sector data == CPCEC)', sa == sc, str([k for k in sa if sa.get(k) != sc.get(k)][:8]))
            for x0, x1 in rng[:6]:
                print('    @%05X arnold %s cpcec %s' % (x0, a_img[x0:x1].hex(), c_img[x0:x1].hex()))

    # 2. FORMAT: drive B formatted to DATA, all 40 tracks, by the FDC (FMTALL)
    for n, src, fmt_change in (('fmt_data', 'std_data.dsk', False), ('fmt_system', 'std_system.dsk', True)):
        b = fresh(G(src), D(n + '.dsk'))
        orig = open(b, 'rb').read()
        log = wb_run(n, [(4, 'KEY:F1'), (7, 'RUN"FMTALL\n'), (30, 'SNAP')], 31, diska=D('tool_fmt.dsk'), diskb=b)
        new = open(b, 'rb').read()
        tr = read_image(b)
        ok = all([(q[0], q[2], q[3], len(q[6]), set(q[6])) for q in tr[(t, 0)]] == [(t, 0xC1 + i, 2, 512, {0xE5}) for i in range(9)] for t in range(40))
        check(n + ' formatted to DATA', ok)
        check(n + ' still standard, same size', new[:8] == b'MV - CPC' and len(new) == len(orig))
        check(n + ' .bak', open(b + '.bak', 'rb').read() == orig)
        log = wb_run(n + '_use', [(4, 'KEY:F1'), (7, '|B\n'), (9, 'CAT\n'), (12, '10 REM FORMATTED\n'), (15, 'SAVE"AFTER\n'),
                     (19, 'CAT\n'), (22, 'SNAP')], 23,
                     diska=D('tool_fmt.dsk'), diskb=b)
        files = amsdos_dir(b)
        check(n + ' usable after FORMAT', sorted(files) == ['AFTER.BAS'], str(sorted(files)))
    # 2b. FORMAT to a new layout (5 x 1K on track 39, a new track 41): becomes extended
    f = fresh(G('std_fmt.dsk'), D('fmt_layout.dsk'))
    log = wb_run('fmt_layout', [(4, 'KEY:F1'), (7, 'RUN"FMTTEST\n'), (16, 'SNAP')], 17, diska=f)
    tr = read_image(f)
    t39 = [(q[2], q[3], len(q[6]), set(q[6])) for q in tr.get((39, 0), [])]
    t41 = [(q[2], q[3], len(q[6]), set(q[6])) for q in tr.get((41, 0), [])]
    ok = t39 == [(0x11 + i, 3, 1024, {0x5A}) for i in range(5)] and t41 == [(0xC1 + i, 2, 512, {0x5A}) for i in range(9)]
    check('new layout: extended, T39 + T41', open(f, 'rb').read()[:8] == b'EXTENDED' and ok, log.strip().splitlines()[-1])
    so, sn = sectors(f + '.bak'), sectors(f)
    check('new layout: other tracks kept', all(sn.get(k) == v for k, v in so.items() if k[0] not in (39, 41)))

    # 3. high score: BIOS sector writes on an extended disk with a weak sector
    e = fresh(G('ext_edge.dsk'), D('hs_edge.dsk'))
    orig = open(e, 'rb').read()
    log = wb_run('hiscore', [(4, 'KEY:F1'), (7, 'MEMORY &3FFF:LOAD"HISCORE.BIN":CALL &4000\n'), (16, 'SNAP')], 17,
                 diska=D('tool_hs.dsk'), diskb=e)
    new = open(e, 'rb').read()
    rng = diff_ranges(orig, new) if len(orig) == len(new) else None
    s = sectors(e)
    ok = rng is not None and s[(20, 0, 0xC5)].startswith(b'HISCORE 12345 JOHN') and s[(39, 0, 0xC2)].startswith(b'HISCORE 2 KEVIN')
    check('hiscore sectors written', ok, str(rng))
    # every changed byte is inside those two sectors' data
    im = read_image(e)
    def span(t, r):
        # offset of the sector's data in the file (extended: block sizes from the table)
        off = 0x100
        for i in range(t): off += orig[0x34 + i] << 8
        th = orig[off:off + 0x100]; p = off + 0x100
        for j in range(th[0x15]):
            l = th[0x18 + 8 * j + 6] | th[0x18 + 8 * j + 7] << 8
            if th[0x18 + 8 * j + 2] == r: return (p, p + 512)
            p += l
    allowed = [span(20, 0xC5), span(39, 0xC2)]
    check('only those bytes changed', rng is not None and all(any(a0 <= x0 and x1 <= a1 for a0, a1 in allowed) for x0, x1 in rng))
    w = [q for q in im[(39, 0)] if q[2] == 0xC1][0]
    check('weak sector copies kept', len(w[6]) == 1536 and orig.find(w[6]) > 0, '%d bytes stored' % len(w[6]))

    # 4. write-protected file: not touched (immutable file on the Linux side; root ignores modes)
    wd = '/opt/src/dskwb-test'; os.makedirs(wd, exist_ok=True)
    p = os.path.join(wd, 'wp.dsk')
    subprocess.run(['chattr', '-i', p], capture_output=True)
    fresh(G('blank_std.dsk'), p)
    orig = open(p, 'rb').read()
    subprocess.run(['chattr', '+i', p], check=True)
    log = wb_run('wprot', save[:3] + [(15, 'SNAP')], 16, diska=p)
    subprocess.run(['chattr', '-i', p])
    check('write-protected: not written', 'write-protected' in log and open(p, 'rb').read() == orig, [l for l in log.splitlines() if 'write-back' in l])
    check('write-protected: no .bak/.tmp', not any(os.path.exists(p + x) for x in ('.bak', '.tmp', '.old')))

    # 5. nothing written when nothing changed (CAT only)
    d = fresh(G('std_data.dsk'), D('readonly_use.dsk'))
    orig = open(d, 'rb').read()
    log = wb_run('noop', [(4, 'KEY:F1'), (7, 'CAT\n'), (10, 'RUN"HELLO\n'), (14, 'SNAP')], 15, diska=d)
    check('no change: file and no .bak', open(d, 'rb').read() == orig and not os.path.exists(d + '.bak'))

    print('\nSUMMARY: %d/%d PASS' % (sum(1 for _, ok in results if ok), len(results)))
    for n, ok in results:
        if not ok: print('  FAIL', n)
    return 0 if all(ok for _, ok in results) else 1

if __name__ == '__main__':
    sys.exit(main())
