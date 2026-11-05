#!/usr/bin/env python3
"""0.11 in digiemu: the EXPRESSION settings kept with the project as text files,
on the firmware and its real +Drive code. Saves the project as 003, loads it and the
factory project back through SETTINGS > PROJECT, and plays a power-on (the
mod's RAM cleared, the file read back from the card).

    set -a; . toolchain/emu-NAME.env; set +a
    digiemu/.venv/bin/python scripts/emu-store.py
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dtsession
from emu.session import Session, run_script

syx = os.environ['DT2_SYX']; snap = os.path.abspath(os.environ['DT2_SNAPSHOT'])
os.chdir(os.path.dirname(syx))
io = dtsession.Midi()
s = Session(snap, syx, audio=False, on_machine=io.on_machine)
uc = s.m.uc
fails = []


def check(ok, what):
    print('%s %s' % ('PASS' if ok else 'FAIL', what))
    if not ok:
        fails.append(what)


def do(text):
    for st in dtsession.parse(text):
        run_script(s, [st])


s.run_ms(300)
mem = bytes(uc.mem_read(0x47be0000, 0x20000))
probe = 0x47be0000 + mem.find(struct.pack('>I', 0x4d455850))
assert probe > 0x47be0000, 'the probe not found'
midi_cfg = 0x47be0000 + mem.find(bytes([0, 1, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0]) * 8)


def pr():   # marker, booted, written, failed, read, bad, slot + 1
    return struct.unpack('>7I', bytes(uc.mem_read(probe, 28)))


def cfg(m):
    return struct.unpack('>hbbbbhhh', bytes(uc.mem_read(midi_cfg + 12 * m, 12)))


def set_cfg(m, thru=1, mw=0, mwa=0, sens=100):
    uc.mem_write(midi_cfg + 12 * m, struct.pack('>hbbbbhhh', thru, mw, 0, 0, sens, mwa, 0, 0))


print('probe %08x, settings %08x' % (probe, midi_cfg))
check(pr()[1] == 1, 'the store was read at power-on (drive mounted)')
saves = pr()[2]
set_cfg(0, mw=7, mwa=20 * 256)
for ms in range(0, 8000, 100):
    s.run_ms(100)
    if pr()[2] > saves:
        break
print('current.txt written after %d ms' % (ms + 100))
check(pr()[2] > saves, 'a change: current.txt written once nothing plays (%d saves, %d failed)' % (pr()[2], pr()[3]))
# SAVE PROJECT AS 003
do('tap GLOBAL\nwait 500\ntap YES\nwait 500\ntap DOWN\nwait 200\ntap YES\nwait 800\ntap DOWN\nwait 300\ntap DOWN\nwait 300\ntap YES\nwait 800\ntap YES\nwait 6000\n')
s.run_ms(2000)
check(pr()[6] == 3, 'SAVE PROJECT AS 003: the mod got slot 003 (%d)' % pr()[6])
check(pr()[2] > saves, 'and wrote its file (%d written)' % pr()[2])
saves = pr()[2]
# change it, then LOAD PROJECT 003
set_cfg(0, mw=2, mwa=0)
s.run_ms(1500)
do('tap NO\nwait 300\ntap NO\nwait 300\ntap GLOBAL\nwait 500\ntap YES\nwait 500\ntap UP\nwait 200\ntap UP\nwait 200\ntap YES\nwait 800\n')   # the menu keeps its cursor: up to LOAD PROJECT
do('tap YES\nwait 800\n')
do('tap YES\nwait 300\n')
do('wait 30000\n')
c = cfg(0)
check(c[1] == 7 and c[5] == 20 * 256, 'LOAD PROJECT 003: MW -> VAL3 +20 back as saved %s' % (c,))
check(pr()[6] == 3, 'the load job gave slot 003 (%d)' % pr()[6])
# LOAD the factory project (no record): the defaults
do('tap NO\nwait 300\ntap NO\nwait 300\ntap GLOBAL\nwait 500\ntap YES\nwait 500\ntap UP\nwait 200\ntap UP\nwait 200\ntap YES\nwait 800\ntap UP\nwait 300\ntap UP\nwait 300\n')
do('tap YES\nwait 800\n')
do('tap YES\nwait 30000\n')
c = cfg(0)
check(c[1] == 0 and c[0] == 1 and c[4] == 100, 'LOAD PROJECT 001 (never saved with the mod): the defaults %s' % (c,))
check(pr()[6] == 1, 'the load job gave slot 001 (%d)' % pr()[6])
# an edit, then a power-on
set_cfg(5, sens=40)
s.run_ms(1500)
for m in range(8):
    set_cfg(m)
uc.mem_write(probe + 4, struct.pack('>I', 0))   # the mod reads its files as at power-on
s.run_ms(1500)
c = cfg(5)
check(pr()[1] == 1 and c[4] == 40, 'power-on: the file read back, as it was at power-off (track F V.SNS %d)' % c[4])
check(not pr()[3], 'no failed write (%d)' % pr()[3])
check(not pr()[5], 'no bad file read (%d)' % pr()[5])
print('%d failures' % len(fails))
