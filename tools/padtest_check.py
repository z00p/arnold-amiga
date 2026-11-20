#!/usr/bin/env python3
"""padtest_check.py [HD] [OUTDIR]: the pad test disk in the headless host (run in WSL).
Builds PadTest.dsk (make_padtest.py), types RUN"PADTEST on the 6128 Plus, then holds
each direction and fire button through the key script and SNAPs it; PNGs in OUTDIR.
HD: tools/dsk_build.sh's host (A=<this tree> D=/opt/src/n21-hd)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dsk_tests, make_padtest

hd = sys.argv[1] if len(sys.argv) > 1 else '/opt/src/n21-hd/hd'
out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(dsk_tests.A, 'build', 'padtest')
os.makedirs(out, exist_ok=True)
dsk = os.path.join(out, 'PadTest.dsk')
make_padtest.main(dsk)
steps = [(4, 'KEY:F1'), (7, 'RUN"PADTEST\n'), (16, 'SNAP')]
t = 17
for k in ('JOYUP', 'JOYDOWN', 'JOYLEFT', 'JOYRIGHT', 'FIRE1', 'FIRE2'):
    steps += [(t, 'KEY:' + k + ' 50'), (t + 0.6, 'SNAP')]
    t += 2
steps.append((t, 'KEY:JOYRIGHT 50'))   # two at once
steps.append((t, 'KEY:FIRE1 50'))
steps.append((t + 0.6, 'SNAP'))
# dsk_tests.keyscript: KEY:<name> is held 5 frames; give the joystick its own length
ks = dsk_tests.keyscript(steps, t + 2).replace(' 50 5\n', ' 50\n')
open(os.path.join(out, 'pad.keys'), 'w').write(ks)
orig = dsk_tests.keyscript
dsk_tests.keyscript = lambda s, e: ks
print(dsk_tests.run(hd, out, 'pad', steps, t + 2, diska=dsk))
dsk_tests.keyscript = orig
