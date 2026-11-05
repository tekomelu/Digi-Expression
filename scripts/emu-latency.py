#!/usr/bin/env python3
"""Latency of live pitch bend to MIDI track A, in digiemu (0.9). Run like emu-midi-whacky.py."""
import os, sys
sys.path.insert(0, 'scripts')
import dtsession
from emu.session import Session, run_script
syx=os.environ['DT2_SYX']; snap=os.path.abspath(os.environ['DT2_SNAPSHOT'])
os.chdir(os.path.dirname(syx))
io=dtsession.Midi()
s=Session(snap, syx, audio=False, on_machine=io.on_machine)
s.run_ms(200)
for st in dtsession.parse('press TRK 100\ntap 9\nrelease TRK 100\nwait 200\npress FUNC 100\ntap A\nrelease FUNC 100\nwait 300\n'):
    run_script(s,[st])
lat=[]
for v in range(20):
    n=len(io.out)
    io.send(s, bytes([0xED, v*5, 64 + v]))   # auto channel 14 (0-based 13)
    for ms in range(1,40):
        s.run_ms(1)
        if len(io.out)>n: lat.append(ms); break
    else: lat.append(None)
    s.run_ms(15)
print('latency ms', lat, io.out[-1] if io.out else None)
