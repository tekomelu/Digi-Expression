#!/usr/bin/env python3
"""Which of a MIDI track's 53 parameter words a project save writes (digiemu): fills MIDI track A's block in the pattern (0x421c9144 -> +0x59a, 112 bytes a track) with markers, saves the project to slot 002 through SETTINGS > PROJECT > SAVE PROJECT AS, and looks for them in what the card was sent. Screens and the written bytes go to results/kitstore/."""
import os, sys, struct
sys.path.insert(0, 'scripts')
import dtsession
from emu.session import Session, run_script
from emu.panel import write_png
syx=os.environ['DT2_SYX']; snap=os.path.abspath(os.environ['DT2_SNAPSHOT'])
out=os.path.abspath('results/kitstore'); os.makedirs(out, exist_ok=True); os.chdir(os.path.dirname(syx))
io=dtsession.Midi()
s=Session(snap, syx, audio=False, on_machine=io.on_machine)
s.run_ms(200)
def shot(name):
    write_png(s.screen_at(s.now_ms()), '%s/%s.png' % (out, name), scale=2)
def do(text):
    for st in dtsession.parse(text):
        run_script(s,[st])
uc=s.m.uc
pat=struct.unpack('>I', bytes(uc.mem_read(0x421c9144,4)))[0]
blk=pat+0x59a
print('pattern %08x, MIDI A block %08x' % (pat, blk))
print('A block:', bytes(uc.mem_read(blk,112)).hex())
orig=bytes(uc.mem_read(blk,112))
for i in range(53):
    uc.mem_write(blk+2*i, bytes([0xA0 + (i >> 4), 0x10 * (i & 15) + 0x0b]))   # distinct, recognisable
uc.mem_write(blk+106, bytes([0xC6, 0xAB]))
do('tap GLOBAL\nwait 500\ntap YES\nwait 500\ntap DOWN\nwait 200\ntap YES\nwait 800\n'); shot('d-saveas')
do('tap DOWN\nwait 300\ntap YES\nwait 800\n'); shot('e-name')
do('tap YES\nwait 4000\n'); shot('f-saved')
do('wait 4000\n'); shot('g-after')
print('A block after:', bytes(uc.mem_read(blk,112)).hex())
card=s.ev['esdhc'].card
ov=card.overlay
print('overlay bytes', len(ov), 'path', getattr(card,'path',None))
keys=sorted(ov)
runs=[]; st=keys[0]; pv=st
for k in keys[1:]:
    if k!=pv+1: runs.append((st,pv+1)); st=k
    pv=k
runs.append((st,pv+1))
print('runs', len(runs), [(hex(a),b-a) for a,b in runs[:12]])
import pickle
blob={a: bytes(ov[i] for i in range(a,b)) for a,b in runs}
pickle.dump(blob, open(os.path.join(os.environ.get('TMPDIR', '/tmp'), 'overlay.pkl'), 'wb'))   # 7 MB: not kept
pat=bytes.fromhex('a00b')
for a,d in blob.items():
    i=d.find(pat)
    while i!=-1:
        print('marker at', hex(a+i), 'run', hex(a), 'ctx', d[max(0,i-84):i+40].hex()); i=d.find(pat,i+1)
