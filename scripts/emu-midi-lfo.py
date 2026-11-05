#!/usr/bin/env python3
"""0.9.3 in digiemu: MIDI track A's LFO as a destination (wheel -> LFO SPD +16).
The mod adds the route to the LFO's words only while the LFO function
(0x400ed438) runs; outside it the track's parameters must stay as they were,
and the unit must run on. Whether the LFO moves differently is for a unit:
digiemu cannot PLAY, and its EMAC handler keeps the function from being
called from a script.

    set -a; . toolchain/emu-NAME.env; set +a
    digiemu/.venv/bin/python scripts/emu-midi-lfo.py
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dtsession
from emu.session import Session, run_script
from unicorn.m68k_const import UC_M68K_REG_A7

P, BODY = 0x421c9188, 0x400ed440
syx = os.environ['DT2_SYX']; snap = os.path.abspath(os.environ['DT2_SNAPSHOT'])
os.chdir(os.path.dirname(syx))
io = dtsession.Midi()
s = Session(snap, syx, audio=False, on_machine=io.on_machine)
uc = s.m.uc
s.run_ms(200)
for st in dtsession.parse('press TRK 100\ntap 9\nrelease TRK 100\nwait 200\npress FUNC 100\ntap A\nrelease FUNC 100\nwait 300\n'):
    run_script(s, [st])
mem = bytes(uc.mem_read(0x47be0000, 0x20000))
midi_cfg = 0x47be0000 + mem.find(bytes([0, 1, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0]) * 8)
uc.mem_write(midi_cfg, struct.pack('>hbbbbhhh', 1, 13, 0, 0, 100, 16 * 256, 0, 0))   # THRU, MW -> LFO SPD +16
import unicorn.m68k_const as K
from unicorn.m68k_const import UC_M68K_REG_PC, UC_M68K_REG_SR
REGS = [getattr(K, n) for n in dir(K) if n.startswith('UC_M68K_REG_') and n[12:13] in 'AD' and len(n) == 14] + [K.UC_M68K_REG_SR]
ENTRY, MAGIC = 0x400ed438, 0x40000400


def spd():
    return struct.unpack('>h', bytes(uc.mem_read(P + 2, 2)))[0]


def run(start, until):
    stepper = getattr(s.m, '_fast_stepper_obj', None)
    left = getattr(stepper, 'left', None)
    if stepper is not None:
        stepper.left = 1 << 40
    try:
        uc.emu_start(start, until, count=400000)
    finally:
        if stepper is not None:
            stepper.left = left
    return uc.reg_read(UC_M68K_REG_PC)


STATE = 0x421f4b90        # the MIDI LFOs' state, 80 bytes a track (0x400ed468)


def state(track):
    return struct.unpack('>20i', bytes(uc.mem_read(STATE + 80 * track, 80)))


def call_lfo(track):
    """The MIDI tracks' task's call, 0x400ed438(params, track, 0): -> returned,
    SPD after, the change in the track's LFO state."""
    saved = {r: uc.reg_read(r) for r in REGS}
    sp = uc.reg_read(UC_M68K_REG_A7) - 0x400
    for i, a in enumerate((MAGIC, P, track, 0)):
        uc.mem_write(sp + 4 * i, struct.pack('>I', a))
    uc.reg_write(UC_M68K_REG_A7, sp)
    uc.reg_write(UC_M68K_REG_SR, 0x2700)
    st0 = state(track)
    pc = run(ENTRY, MAGIC)
    st1 = state(track)
    for r, v in saved.items():
        try:
            uc.reg_write(r, v)
        except Exception:
            pass
    return pc == MAGIC, spd(), [b - a for a, b in zip(st0, st1)]


# The firmware's EMAC instructions run through digiemu's handler, which
# restarts the CPU: the function cannot be called from here. It runs on its
# own, about 30 times a second (the task's clock record): watch its state.
mem = bytes(uc.mem_read(0x47be0000, 0x20000))
probe = 0x47be0000 + mem.find(struct.pack('>I', 0x4d454c46))


def probe_counts():
    return struct.unpack('>II', bytes(uc.mem_read(probe + 4, 8)))


fails = 0
base = spd()
adv = {}
c0 = probe_counts()
for wheel in (0, 127, 0, 127):
    io.send(s, bytes([0xBD, 1, wheel])); s.run_ms(50)
    st0 = state(0); s.run_ms(300); st1 = state(0)
    d = [(b - a) & 0xffffffff for a, b in zip(st0, st1)]
    adv.setdefault(wheel, []).append(d)
    ok = spd() == base
    fails += not ok
    print('%s wheel %3d: SPD outside the LFO %04x (was %04x); state change in 300 ms %s' % ('PASS' if ok else 'FAIL', wheel, spd() & 0xffff, base & 0xffff, ['%08x' % x for x in d if x][:4]))
c1 = probe_counts()
print('INFO the LFO site ran %d times (through the mod), %d with the wheel route in; marker at %08x' % (c1[0] - c0[0], c1[1] - c0[1], probe))
moved = adv[127][0] != adv[0][0]
print('INFO the LFO state %s (digiemu cannot PLAY; the MIDI LFO may not run here at all: check on a unit)' % ('changes with the wheel' if moved else 'does not change'))
s.run_ms(500)
print('%s the unit runs on' % ('PASS' if s.now_ms() > 0 else 'FAIL'))
print('%d failures' % fails)
