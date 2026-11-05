#!/usr/bin/env python3
"""0.9.2 in digiemu: before MIDI track A sends anything, live input adds to its parameters (MW 10, PB 12288)."""
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
P=0x421c9188
print('params 21..32 track A:', [s.m.uc.mem_read(P+2*i,2).hex() for i in range(21,33)])
s.m.uc.mem_write(P+2*23, bytes([10,0])); s.m.uc.mem_write(P+2*21, bytes([0x30,0]))     # MW 10, as the knob would
n=len(io.out); io.send(s, bytes([0xBD,1,50])); s.run_ms(20)
print('wheel 50 ->', [d.hex() for _,d in io.out[n:]])
n=len(io.out); io.send(s, bytes([0xED,0,0x41])); s.run_ms(20); print('bend +128 ->', [d.hex() for _,d in io.out[n:]])
