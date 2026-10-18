#!/usr/bin/env python3
"""Test disk images for the Arnold disk path (tools/dsk_tests.py drives them).

Writes standard ("MV - CPCEMU") and extended ("EXTENDED CPC DSK") images with
AMSDOS files built here: a tokenised BASIC program, an ASCII BASIC program,
small Z80 binaries, a 16K screen (multi-extent file) and FDCTEST.BIN, a
direct-FDC probe that reads IDs and sectors on chosen tracks and prints the
result bytes. Edge-case tracks (odd sector sizes, ID gaps, unformatted, weak
copies, short 8K sector, foreign C/H) go after the AMSDOS file area.

    python3 make_dsk.py OUTDIR
Everything is generated; no third-party data."""
import os, struct, sys

# ---------------------------------------------------------------- Z80 -----
class Asm:
    """Just enough of an assembler: raw bytes, labels and fix-ups."""
    def __init__(self, org):
        self.org, self.b, self.labels, self.fix = org, bytearray(), {}, []
    def pc(self): return self.org + len(self.b)
    def L(self, name): self.labels[name] = self.pc()
    def db(self, *v):
        for x in v:
            if isinstance(x, (bytes, bytearray)): self.b += x
            elif isinstance(x, str): self.b += x.encode('latin-1')
            else: self.b.append(x & 255)
    def dw(self, v):
        if isinstance(v, str): self.fix.append(('w', len(self.b), v)); self.b += b'\0\0'
        else: self.b += struct.pack('<H', v & 0xffff)
    def jr(self, op, label): self.db(op); self.fix.append(('r', len(self.b), label)); self.b.append(0)
    def op_w(self, op, v):          # opcode(s) + 16-bit operand
        self.db(*op) if isinstance(op, tuple) else self.db(op); self.dw(v)
    def build(self):
        for kind, at, lab in self.fix:
            a = self.labels[lab]
            if kind == 'w': self.b[at:at + 2] = struct.pack('<H', a)
            else:
                d = a - (self.org + at + 1)
                assert -128 <= d < 128, lab
                self.b[at] = d & 255
        return bytes(self.b)

TXT_OUTPUT, SCR_SET_MODE, SCR_SET_BORDER, KM_WAIT_KEY = 0xBB5A, 0xBC0E, 0xBC38, 0xBB18

def emit_print(a, label):          # print a 0-terminated string at label (uses HL, A)
    a.op_w(0x21, label)
    lp = 'p%d' % len(a.b); dn = 'q%d' % len(a.b)
    a.L(lp); a.db(0x7E, 0xB7); a.jr(0x28, dn)          # ld a,(hl); or a; jr z,done
    a.op_w(0xCD, TXT_OUTPUT); a.db(0x23); a.jr(0x18, lp)
    a.L(dn)

def prog_print():
    """PRINT.BIN at &4000: prints a line and returns (LOAD + CALL &4000)."""
    a = Asm(0x4000)
    emit_print(a, 'msg'); a.db(0xC9)
    a.L('msg'); a.db('BINARY CALL OK', 13, 10, 0)
    return a.build()

def prog_loop(text='RUN BINARY OK', border=18):
    """LOOP.BIN at &4000: mode 1, green border, message, stripes, loops forever."""
    a = Asm(0x4000)
    a.db(0x3E, 1); a.op_w(0xCD, SCR_SET_MODE)
    a.db(0x06, border, 0x0E, border); a.op_w(0xCD, SCR_SET_BORDER)
    emit_print(a, 'msg')
    # stripes in the bottom character row block: &C000+&780.. 80 bytes x 8 lines
    a.op_w(0x21, 0xC780); a.db(0x0E, 8)                 # ld hl,&C780; ld c,8
    a.L('row'); a.db(0x06, 80, 0xE5)                    # ld b,80; push hl
    a.L('col'); a.db(0x36, 0xF0, 0x23); a.jr(0x10, 'col')   # ld (hl),&F0; inc hl; djnz
    a.db(0xE1); a.op_w(0x11, 0x0800); a.db(0x19, 0x0D); a.jr(0x20, 'row')  # pop hl; add hl,&800; dec c
    a.L('end'); a.jr(0x18, 'end')
    a.L('msg'); a.db(text, 13, 10, 0)
    return a.build()

def prog_boot(text='CPM BOOT SECTOR OK'):
    """Boot sector for |CPM (track 0 sector &41, run at &0100)."""
    a = Asm(0x0100)
    a.db(0x3E, 1); a.op_w(0xCD, SCR_SET_MODE)
    a.db(0x06, 9, 0x0E, 9); a.op_w(0xCD, SCR_SET_BORDER)   # green border
    emit_print(a, 'msg')
    a.L('end'); a.jr(0x18, 'end')
    a.L('msg'); a.db(text, 13, 10, 0)
    b = a.build()
    return b + bytes(512 - len(b))

def prog_fdctest(tests, drive=0):
    """FDCTEST.BIN at &4000: drives the uPD765 directly with interrupts off
    (so the AMSDOS motor timer can't interfere), then prints one line a test:
      tt hRR: s0 s1 s2 C  H  R  N  count sum
    tests: list of (kind, track, head, C, H, R, N) with kind 'id' (READ ID) or
    'rd' (READ DATA of sector R, EOT=R). count = bytes moved in the execution
    phase, sum = 8-bit sum of them."""
    a = Asm(0x4000)
    tab = 0x5000                     # 8-byte test records, then results
    res = 0x5400                     # 16 bytes a test
    buf = 0x6000                     # sector data (up to 16K)
    a.db(0xF3)                                               # di
    a.op_w((0x01,), 0xFA7E); a.db(0x3E, 1, 0xED, 0x79)       # motor on
    a.op_w((0x11,), 0x0000)                                  # ~0.5 s busy wait
    a.L('mw'); a.db(0x1B, 0x7A, 0xB3); a.jr(0x20, 'mw')      # dec de; ld a,d; or e; jr nz
    a.op_w((0xDD, 0x21), tab)                                # ix = test table
    a.op_w((0xFD, 0x21), res)                                # iy = results
    a.L('next')
    a.db(0xDD, 0x7E, 0); a.db(0xB7); a.op_w(0xCA, 'alldone')                                    # kind 0 = end
    # recalibrate (the FDC's idea of the head position starts at 0 whatever the drive's)
    a.db(0x3E, 0x07); a.op_w(0xCD, 'out')
    a.db(0x3E, drive); a.op_w(0xCD, 'out')
    a.L('snr'); a.db(0x3E, 0x08); a.op_w(0xCD, 'out')
    a.op_w(0xCD, 'in'); a.db(0x5F); a.op_w(0xCD, 'in')
    a.db(0xCB, 0x6B); a.jr(0x28, 'snr')
    # seek: 0F, drive|head<<2, track ; then sense interrupt until seek end
    a.db(0x3E, 0x0F); a.op_w(0xCD, 'out')
    a.db(0xDD, 0x7E, 2, 0x87, 0x87, 0xF6, drive); a.op_w(0xCD, 'out')
    a.db(0xDD, 0x7E, 1); a.op_w(0xCD, 'out')
    a.L('sns'); a.db(0x3E, 0x08); a.op_w(0xCD, 'out')
    a.op_w(0xCD, 'in'); a.db(0x5F); a.op_w(0xCD, 'in')       # st0 -> e, pcn
    a.db(0xCB, 0x6B); a.jr(0x28, 'sns')                      # bit 5,b: seek end?
    a.db(0x21, 0, 0, 0x22); a.dw(0x53F0)                     # count = 0
    a.db(0xAF, 0x32); a.dw(0x53F2)                           # sum = 0
    a.db(0xDD, 0x7E, 0, 0xFE, 3); a.op_w(0xCA, 'fmt')         # kind 3: FORMAT TRACK
    a.db(0xDD, 0x7E, 0, 0xFE, 1); a.jr(0x20, 'rd')
    # READ ID: 4A, drive|head<<2
    a.db(0x3E, 0x4A); a.op_w(0xCD, 'out')
    a.db(0xDD, 0x7E, 2, 0x87, 0x87, 0xF6, drive); a.op_w(0xCD, 'out')
    a.jr(0x18, 'result')
    a.L('rd')                                                # READ DATA 46 d C H R N EOT GPL DTL
    a.db(0x3E, 0x46); a.op_w(0xCD, 'out')
    a.db(0xDD, 0x7E, 2, 0x87, 0x87, 0xF6, drive); a.op_w(0xCD, 'out')
    for off in (3, 4, 5, 6, 5):                              # C H R N EOT(=R)
        a.db(0xDD, 0x7E, off); a.op_w(0xCD, 'out')
    a.db(0x3E, 0x2A); a.op_w(0xCD, 'out')
    a.db(0x3E, 0xFF); a.op_w(0xCD, 'out')
    # execution phase: data while EXM (bit 5) is set
    a.op_w(0x21, buf); a.op_w((0x11,), 0); a.op_w((0x01,), 0xFB7E)
    a.L('ex'); a.db(0xED, 0x78, 0xF2); a.dw('ex')            # in a,(c); jp p,ex (RQM)
    a.db(0xE6, 0x20); a.jr(0x28, 'exd')                      # and &20; jr z,done
    a.db(0x0C, 0xED, 0x78, 0x0D, 0x77, 0x23, 0x13)           # inc c; in a,(c); dec c; ld (hl),a; inc hl; inc de
    a.db(0xE5, 0x21); a.dw(0x53F2); a.db(0x86, 0x77, 0xE1)   # sum += a
    a.jr(0x18, 'ex')
    a.L('exd'); a.db(0xED, 0x53); a.dw(0x53F0)               # ld (count),de
    a.L('result')                                            # 7 result bytes -> (iy+0..6)
    a.db(0x06, 7, 0xFD, 0xE5, 0xE1)                          # ld b,7; push iy; pop hl
    a.L('rl'); a.db(0xC5); a.op_w(0xCD, 'in'); a.db(0xC1, 0x77, 0x23); a.jr(0x10, 'rl')
    a.db(0x2A); a.dw(0x53F0); a.db(0xFD, 0x75, 7, 0xFD, 0x74, 8)   # count -> iy+7/8
    a.db(0x3A); a.dw(0x53F2); a.db(0xFD, 0x77, 9)                   # sum -> iy+9
    a.op_w((0x11,), 8); a.db(0xDD, 0x19)                     # ix += 8
    a.op_w((0x11,), 16); a.db(0xFD, 0x19)                    # iy += 16
    a.op_w(0xC3, 'next')
    # FORMAT TRACK: 4D, d, N, SC, GPL, filler &5A, then C H R N for each sector (R counting up)
    a.L('fmt')
    a.db(0x3E, 0x4D); a.op_w(0xCD, 'out')
    a.db(0xDD, 0x7E, 2, 0x87, 0x87, 0xF6, drive); a.op_w(0xCD, 'out')
    a.db(0xDD, 0x7E, 6); a.op_w(0xCD, 'out')
    a.db(0xDD, 0x7E, 7); a.op_w(0xCD, 'out')
    a.db(0x3E, 0x20); a.op_w(0xCD, 'out')
    a.db(0x3E, 0x5A); a.op_w(0xCD, 'out')
    a.db(0xDD, 0x5E, 7, 0xDD, 0x56, 5)                      # e = SC, d = first R
    a.L('fl')
    a.db(0xDD, 0x7E, 3); a.op_w(0xCD, 'out')
    a.db(0xDD, 0x7E, 4); a.op_w(0xCD, 'out')
    a.db(0x7A); a.op_w(0xCD, 'out')
    a.db(0xDD, 0x7E, 6); a.op_w(0xCD, 'out')
    a.db(0x14, 0x1D); a.jr(0x20, 'fl')                      # inc d; dec e
    a.op_w(0xC3, 'result')
    a.L('alldone')
    a.op_w((0x01,), 0xFA7E); a.db(0xAF, 0xED, 0x79)          # motor off
    a.db(0xFB)                                               # ei
    # print: per test "tt hRR: " + 10 hex bytes
    a.op_w((0xDD, 0x21), tab); a.op_w((0xFD, 0x21), res)
    a.L('pl'); a.db(0xDD, 0x7E, 0, 0xB7); a.op_w(0xCA, 'hang')   # end: wait here (RUN" resets on RET)
    a.db(0xDD, 0x7E, 1); a.op_w(0xCD, 'hex'); a.db(0x3E, 0x20); a.op_w(0xCD, TXT_OUTPUT)
    a.db(0xDD, 0x7E, 2, 0xC6, 0x30); a.op_w(0xCD, TXT_OUTPUT)
    a.db(0xDD, 0x7E, 5); a.op_w(0xCD, 'hex'); a.db(0x3E, 0x3A); a.op_w(0xCD, TXT_OUTPUT)
    a.db(0x06, 10, 0xFD, 0xE5, 0xE1)
    a.L('pb'); a.db(0x3E, 0x20); a.op_w(0xCD, TXT_OUTPUT)
    a.db(0x7E); a.op_w(0xCD, 'hex'); a.db(0x23); a.jr(0x10, 'pb')
    a.db(0x3E, 13); a.op_w(0xCD, TXT_OUTPUT); a.db(0x3E, 10); a.op_w(0xCD, TXT_OUTPUT)
    a.op_w((0x11,), 8); a.db(0xDD, 0x19); a.op_w((0x11,), 16); a.db(0xFD, 0x19)
    a.jr(0x18, 'pl')
    a.L('hang'); a.jr(0x18, 'hang')
    # helpers
    a.L('out'); a.db(0xF5); a.op_w((0x01,), 0xFB7E)          # push af; ld bc,&FB7E
    a.L('ow'); a.db(0xED, 0x78, 0xF2); a.dw('ow')            # wait RQM
    a.db(0xF1, 0x0C, 0xED, 0x79, 0xC9)                       # pop af; inc c; out (c),a; ret
    a.L('in'); a.op_w((0x01,), 0xFB7E)
    a.L('iw'); a.db(0xED, 0x78, 0xF2); a.dw('iw')
    a.db(0xE6, 0x40); a.jr(0x28, 'inone')                    # DIO=0: no more result bytes
    a.db(0x0C, 0xED, 0x78, 0xC9)
    a.L('inone'); a.db(0x3E, 0xEE, 0xC9)                     # 'EE' marks a missing byte
    a.L('hex'); a.db(0xF5, 0x0F, 0x0F, 0x0F, 0x0F); a.op_w(0xCD, 'nib'); a.db(0xF1)
    a.L('nib'); a.db(0xE6, 0x0F, 0xC6, 0x90, 0x27, 0xCE, 0x40, 0x27)  # classic hex digit
    a.op_w(0xC3, TXT_OUTPUT)
    code = a.build()
    assert len(code) < 0x1000
    t = bytearray()
    for x in tests:
        kind, trk, head, C, H, R, N = x[:7]
        t += bytes([{'id': 1, 'rd': 2, 'fmt': 3}[kind], trk, head, C, H, R, N, x[7] if len(x) > 7 else 0])
    t += bytes(8)
    return code + bytes(0x1000 - len(code)) + bytes(t)      # code at &4000, table at &5000

# --------------------------------------------------------------- BASIC ----
T = {'MODE': 0xAD, 'PRINT': 0xBF, 'BORDER': 0x82, 'CLS': 0x8A}
def basic_line(num, toks):
    body = bytearray()
    for t in toks:
        if isinstance(t, int): body.append(t)
        elif t in T: body.append(T[t])
        else: body += t.encode('latin-1')
    body.append(0)
    return struct.pack('<HH', len(body) + 4, num) + body

def prog_basic():
    """10 MODE 1:BORDER 6 / 20 PRINT "TOKENISED BASIC OK" / 30 PRINT 6*7"""
    p = basic_line(10, ['MODE', ' ', 0x0F, 0x01, 'BORDER', ' ', 0x14])
    p += basic_line(20, ['PRINT', ' "TOKENISED BASIC OK"'])
    p += basic_line(30, ['PRINT', ' ', 0x14, 0xF6, 0x15])
    return p + b'\0\0'

# -------------------------------------------------------------- AMSDOS ----
def amsdos_header(name, ftype, load, data, entry=0):
    n, _, e = name.upper().partition('.')
    h = bytearray(128)
    h[1:9] = n.ljust(8).encode(); h[9:12] = e.ljust(3).encode()
    h[18] = ftype
    struct.pack_into('<H', h, 21, load); h[23] = 0xFF
    struct.pack_into('<H', h, 24, len(data)); struct.pack_into('<H', h, 26, entry)
    h[64:67] = struct.pack('<I', len(data))[:3]
    struct.pack_into('<H', h, 67, sum(h[:67]) & 0xffff)
    return bytes(h)

class Amsdos:
    """An AMSDOS file system on 9 x 512-byte sectors a track (DATA or SYSTEM)."""
    def __init__(self, fmt='DATA', tracks=40):
        self.base, self.res = (0xC1, 0) if fmt == 'DATA' else (0x41, 2)
        self.tracks = tracks
        self.sec = {}                                   # (track, R) -> bytes
        self.dir = []
        self.nextblock = 2                              # blocks 0,1 = directory
        self.maxblock = ((tracks - self.res) * 9) // 2 - 1
    def put_block(self, blk, data):
        for i in range(2):
            s = blk * 2 + i
            self.sec[(self.res + s // 9, self.base + s % 9)] = data[i * 512:(i + 1) * 512].ljust(512, b'\x1a')
    def add(self, name, data, header=None, user=0):
        """header=(type, load, entry) for a file with an AMSDOS header; None = ASCII."""
        raw = (amsdos_header(name, header[0], header[1], data, header[2]) + data) if header else data
        recs = (len(raw) + 127) // 128
        raw = raw.ljust(recs * 128, b'\x1a')
        blocks = []
        for off in range(0, len(raw), 1024):
            assert self.nextblock <= self.maxblock, 'disk full'
            blocks.append(self.nextblock); self.put_block(self.nextblock, raw[off:off + 1024]); self.nextblock += 1
        n, _, e = name.upper().partition('.')
        for ex in range(0, max(1, (len(blocks) + 15) // 16)):
            al = blocks[ex * 16:(ex + 1) * 16]
            rc = min(128, recs - ex * 128)
            ent = bytes([user]) + n.ljust(8).encode() + e.ljust(3).encode() + bytes([ex, 0, 0, rc]) + bytes(al).ljust(16, b'\0')
            self.dir.append(ent)
    def sectors(self):
        d = b''.join(self.dir).ljust(2048, b'\xe5')
        self.put_block(0, d[:1024]); self.put_block(1, d[1024:])
        return self.sec

# ---------------------------------------------------------------- images --
def std_track(fs, t, interleave):
    ids = [fs.base + i for i in (interleave or range(9))]
    return [(t, 0, r, 2, 0, 0, fs.sec.get((t, r), b'\xe5' * 512)) for r in ids]

ILV = [0, 5, 1, 6, 2, 7, 3, 8, 4]        # AMSDOS-style physical order C1 C6 C2 C7 ...

def write_dsk(path, tracks, sides=1):
    """tracks: list (track-major, side-minor) of sector lists [(C,H,R,N,ST1,ST2,data)]."""
    size = 256 + max(sum(len(s[6]) for s in t) for t in tracks if t)
    size = (size + 255) & ~255
    out = bytearray(b'MV - CPCEMU Disk-File\r\nDisk-Info\r\n'.ljust(34, b'\0') + b'make_dsk.py'.ljust(14, b'\0'))
    out += bytes([len(tracks) // sides, sides]) + struct.pack('<H', size)
    out = out.ljust(256, b'\0')
    for i, t in enumerate(tracks):
        th = bytearray(b'Track-Info\r\n'.ljust(16, b'\0')) + bytes([i // sides, i % sides, 0, 0, 2, len(t), 0x4E, 0xE5])
        for (C, H, R, N, s1, s2, d) in t: th += bytes([C, H, R, N, s1, s2, 0, 0])
        tr = th.ljust(256, b'\0') + b''.join(s[6] for s in t)
        out += tr.ljust(size, b'\0')
    open(path, 'wb').write(out)

def write_edsk(path, tracks, sides=1, zero_sizes=()):
    """Extended image; an empty sector list = unformatted track (size 0 in the table).
    zero_sizes: track indexes whose sector info carries data length 0 (old EDSK writers)."""
    hdr = bytearray(b'EXTENDED CPC DSK File\r\nDisk-Info\r\n'.ljust(34, b'\0') + b'make_dsk.py'.ljust(14, b'\0'))
    hdr += bytes([len(tracks) // sides, sides, 0, 0])
    body = bytearray(); table = []
    for i, t in enumerate(tracks):
        if not t: table.append(0); continue
        th = bytearray(b'Track-Info\r\n'.ljust(16, b'\0')) + bytes([i // sides, i % sides, 0, 0, max(s[3] for s in t), len(t), 0x4E, 0xE5])
        for (C, H, R, N, s1, s2, d) in t:
            th += bytes([C, H, R, N, s1, s2]) + struct.pack('<H', 0 if i in zero_sizes else len(d))
        tr = th.ljust(512 if len(t) > 29 else 256, b'\0') + b''.join(s[6] for s in t)   # >29 IDs: 512-byte header
        tr = tr.ljust((len(tr) + 255) & ~255, b'\0')
        table.append(len(tr) >> 8); body += tr
    hdr += bytes(table); hdr = hdr.ljust(256, b'\0')
    open(path, 'wb').write(bytes(hdr) + bytes(body))

def pat(seed, n):
    x, out = seed * 2654435761 + 1, bytearray()
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0x7fffffff; out.append((x >> 16) & 255)
    return bytes(out)

def screen16k():
    """A 16K mode-1 screen: diagonal bands, enough to recognise when loaded at &C000."""
    s = bytearray(16384)
    for line in range(200):
        a = (line // 8) * 80 + (line % 8) * 2048
        for x in range(80):
            s[a + x] = 0xF0 if ((x + line // 4) // 4) % 2 else 0x0F
    return bytes(s)

def standard_files(fs, tag):
    fs.add('HELLO.BAS', prog_basic(), (0, 0x170, 0))
    fs.add('ASCII.BAS', b'10 PRINT "ASCII BASIC OK ' + tag.encode() + b'"\r\n20 PRINT 2+3\r\n\x1a')
    fs.add('PRINT.BIN', prog_print(), (2, 0x4000, 0x4000))
    fs.add('LOOP.BIN', prog_loop('RUN BINARY OK ' + tag), (2, 0x4000, 0x4000))
    fs.add('SCREEN.BIN', screen16k(), (2, 0xC000, 0))

def fs_tracks(fs, ilv=None, ntracks=None):
    fs.sectors()
    return [std_track(fs, t, ilv) for t in range(ntracks or fs.tracks)]

def main(out):
    os.makedirs(out, exist_ok=True)
    # 1-2: standard DSK, DATA and SYSTEM
    fs = Amsdos('DATA'); standard_files(fs, 'DSK DATA')
    write_dsk(os.path.join(out, 'std_data.dsk'), fs_tracks(fs, ILV))
    fs = Amsdos('SYSTEM'); standard_files(fs, 'DSK SYS')
    fs.sec[(0, 0x41)] = prog_boot('CPM BOOT OK STD DSK')
    write_dsk(os.path.join(out, 'std_system.dsk'), fs_tracks(fs, ILV))
    # 3-4: extended DSK, DATA (sequential order) and SYSTEM
    fs = Amsdos('DATA'); standard_files(fs, 'EDSK DATA')
    write_edsk(os.path.join(out, 'ext_data.dsk'), fs_tracks(fs))
    fs = Amsdos('SYSTEM'); standard_files(fs, 'EDSK SYS')
    fs.sec[(0, 0x41)] = prog_boot('CPM BOOT OK EDSK')
    write_edsk(os.path.join(out, 'ext_system.dsk'), fs_tracks(fs, ILV))
    # 5: blank formatted DATA disks (write tests), standard and extended
    fs = Amsdos('DATA'); write_dsk(os.path.join(out, 'blank_std.dsk'), fs_tracks(fs, ILV))
    fs = Amsdos('DATA'); write_edsk(os.path.join(out, 'blank_ext.dsk'), fs_tracks(fs, ILV))
    # 6: double-sided extended image (side 1 has its own IDs, H=1); drive B is the double-sided drive
    fs = Amsdos('DATA'); standard_files(fs, 'DS SIDE0')
    side0 = fs_tracks(fs, ILV)
    side1 = [[(t, 1, 0xC1 + i, 2, 0, 0, pat(t * 16 + i + 100, 512)) for i in range(9)] for t in range(40)]
    write_edsk(os.path.join(out, 'ext_ds.dsk'), [x for t in range(40) for x in (side0[t], side1[t])], sides=2)
    # 7: 42-track extended DATA disk with edge-case tracks after the file area (36..41)
    E = {}
    E[36] = [(36, 0, r, 2, 0, 0, pat(r, 512)) for r in (0xC5, 0xC1, 0xC9, 0xC3)]                # out of order, gaps
    E[37] = [(37, 0, 1, 3, 0, 0, pat(1, 1024)), (37, 0, 2, 3, 0, 0, pat(2, 1024)),
             (37, 0, 3, 1, 0, 0, pat(3, 256)), (37, 0, 4, 0, 0, 0, pat(4, 128)),
             (37, 0, 5, 3, 0, 0, pat(5, 1024))]                                                  # mixed sizes
    E[38] = []                                                                                   # unformatted
    E[39] = [(39, 0, 0xC1, 2, 0x20, 0x20, pat(7, 512) + pat(8, 512) + pat(9, 512)),              # weak: 3 copies, CRC error
             (39, 0, 0xC2, 2, 0, 0, pat(10, 512))]
    E[40] = [(40, 0, 0xC1, 6, 0x20, 0x20, pat(11, 6144)), (40, 0, 0xC2, 2, 0, 0, pat(15, 512))]  # 8K ID, 6144 stored
    E[41] = [(0x50, 1, 0x21, 2, 0, 0, pat(12, 512)), (41, 0, 0xC1, 2, 0, 0x40, pat(13, 512))]   # foreign C/H; deleted mark
    fs = Amsdos('DATA', 42)
    tests = [('id', t, 0, 0, 0, 0, 0) for t in range(36, 42)]
    tests += [('rd', 36, 0, 36, 0, 0xC3, 2), ('rd', 36, 0, 36, 0, 0xC2, 2),
              ('rd', 37, 0, 37, 0, 2, 3), ('rd', 37, 0, 37, 0, 3, 1), ('rd', 37, 0, 37, 0, 4, 0),
              ('rd', 38, 0, 38, 0, 0xC1, 2),
              ('rd', 39, 0, 39, 0, 0xC1, 2), ('rd', 39, 0, 39, 0, 0xC2, 2),
              ('rd', 40, 0, 40, 0, 0xC2, 2),
              ('rd', 41, 0, 0x50, 1, 0x21, 2), ('rd', 41, 0, 41, 0, 0xC1, 2),
              ('rd', 0, 0, 0, 0, 0xC1, 2)]
    fs.add('FDCTEST.BIN', prog_fdctest(tests), (2, 0x4000, 0x4000))
    fs.add('HELLO.BAS', prog_basic(), (0, 0x170, 0))
    fs.sectors()
    tr = [std_track(fs, t, ILV) for t in range(36)] + [E[t] for t in range(36, 42)]
    write_edsk(os.path.join(out, 'ext_edge.dsk'), tr)
    open(os.path.join(out, 'ext_edge.expect'), 'w').write(expect_edge(E, tests))
    # 8: same files, old-style EDSK with zero data lengths in the sector info
    fs = Amsdos('DATA'); standard_files(fs, 'EDSK OLD')
    write_edsk(os.path.join(out, 'ext_zerolen.dsk'), fs_tracks(fs, ILV), zero_sizes=set(range(40)))
    # 8b: standard DSK whose last track has IDs with N other than the track's size (2):
    #     the data of every sector is 512 bytes in a standard image
    fs = Amsdos('DATA'); standard_files(fs, 'DSK N MIX')
    tr = fs_tracks(fs, ILV)
    tr[39] = [(39, 0, 0xC1, 2, 0, 0, pat(21, 512)), (39, 0, 0xC2, 3, 0, 0, pat(22, 512)),
              (39, 0, 0xC3, 2, 0, 0, pat(23, 512)), (39, 0, 0xC4, 1, 0, 0, pat(24, 512)),
              (39, 0, 0xC5, 6, 0x20, 0x20, pat(25, 512))]
    write_dsk(os.path.join(out, 'std_nmix.dsk'), tr)
    # 8c: extended image with a 40-sector track (128-byte sectors, 512-byte track header)
    fs = Amsdos('DATA'); standard_files(fs, 'EDSK 40 SEC')
    tr = fs_tracks(fs, ILV)
    tr[39] = [(39, 0, r, 0, 0, 0, pat(r + 30, 128)) for r in range(1, 41)]
    write_edsk(os.path.join(out, 'ext_many.dsk'), tr)
    # 8d: format test: FMTTEST.BIN formats track 39 (5 x N=3, IDs &11-&15) and the new
    #     track 41 (9 x N=2, &C1-&C9) on a 40-track standard image, then reads them back
    fs = Amsdos('DATA')
    fs.add('FMTTEST.BIN', prog_fdctest([('fmt', 39, 0, 39, 0, 0x11, 3, 5), ('id', 39, 0, 0, 0, 0, 0),
                                         ('rd', 39, 0, 39, 0, 0x13, 3), ('fmt', 41, 0, 41, 0, 0xC1, 2, 9),
                                         ('rd', 41, 0, 41, 0, 0xC5, 2), ('rd', 39, 0, 39, 0, 0xC1, 2)]), (2, 0x4000, 0x4000))
    write_dsk(os.path.join(out, 'std_fmt.dsk'), fs_tracks(fs, ILV))
    # 9: AMSDOS-format directory only reached through FDCTEST on drive B side 1
    fs = Amsdos('DATA')
    fs.add('FDCB.BIN', prog_fdctest([('id', 5, 1, 0, 0, 0, 0), ('rd', 5, 1, 5, 1, 0xC3, 2), ('rd', 5, 0, 5, 0, 0xC1, 2)], drive=1),
           (2, 0x4000, 0x4000))
    write_dsk(os.path.join(out, 'std_fdcb.dsk'), fs_tracks(fs, ILV))
    print('written to', out)

def expect_edge(E, tests):
    """Expected count and 8-bit sum for the READ DATA tests (data part only)."""
    lines = []
    for kind, trk, head, C, H, R, N in tests:
        if kind != 'rd': continue
        secs = E.get(trk)
        hit = [s for s in (secs or []) if (s[0], s[1], s[2], s[3]) == (C, H, R, N)] if secs is not None else None
        if hit:
            d = hit[0][6][:128 << N]
            lines.append('%02X %02X: count %04X sum %02X' % (trk, R, len(d), sum(d) & 255))
        else:
            lines.append('%02X %02X: not found' % (trk, R))
    return '\n'.join(lines) + '\n'

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'dsk-test')
