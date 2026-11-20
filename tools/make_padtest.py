#!/usr/bin/env python3
"""make_padtest.py OUT.dsk: the CPC pad test disk shipped with Arnold (2.1).

One ASCII BASIC program, PADTEST.BAS, on an AMSDOS DATA disk: it shows CPC
joystick 0 as games read it (JOY(0): bits 0-3 up, down, left, right, bit 4
Arnold's Fire 1, bit 5 Fire 2), each lit while held, plus the raw value.
Start with RUN"PADTEST (6128 or 6128 Plus). Written here; no third-party data.
tools/padtest_check.py runs it in the headless host."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_dsk

BASIC = r'''10 MODE 1:BORDER 1:INK 0,1:INK 1,24:INK 2,20:INK 3,6
20 PEN 1:LOCATE 13,2:PRINT"ARNOLD PAD TEST"
30 PEN 2:LOCATE 3,4:PRINT"CPC joystick 0, as games read it"
40 LOCATE 2,21:PRINT"Fire 1: red button / joystick fire"
50 LOCATE 2,22:PRINT"Fire 2: blue, green, yellow / button 2"
60 LOCATE 2,24:PRINT"Press ESC twice to stop";
70 DIM n$(5),x(5),y(5),m(5)
80 FOR i=0 TO 5:READ n$(i),x(i),y(i):m(i)=2^i:NEXT
90 DATA "  UP  ",18,8," DOWN ",18,14," LEFT ",8,11," RIGHT ",27,11," FIRE 1 ",8,17," FIRE 2 ",25,17
100 o=-1
110 j=JOY(0):IF j=o THEN 110
120 o=j
130 FOR i=0 TO 5
140 PEN 1:IF j AND m(i) THEN PAPER 3 ELSE PAPER 0
150 LOCATE x(i),y(i):PRINT n$(i);
160 NEXT
170 PEN 2:PAPER 0:LOCATE 15,19:PRINT USING "JOY(0) ###";j;
180 GOTO 110
'''


def disk_image():
    fs = make_dsk.Amsdos('DATA')
    fs.add('PADTEST.BAS', BASIC.replace('\n', '\r\n').encode('latin-1') + b'\x1a')
    return fs


def main(out):
    make_dsk.write_dsk(out, make_dsk.fs_tracks(disk_image(), make_dsk.ILV))
    print(out, os.path.getsize(out))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'PadTest.dsk')
