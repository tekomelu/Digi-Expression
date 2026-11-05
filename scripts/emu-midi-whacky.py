#!/usr/bin/env python3
"""MIDI Expression 0.8 on the real firmware, in digiemu: odd combinations of
live input, routes, merged CC numbers and the track's own sends (research and
regression; see module/midi-expression/TESTING.md).

The emulator cannot run the sequencer or turn knobs, so the MIDI track's own
sends (knob, parameter lock, LFO) are made by calling the firmware's own
send(track, channel, id, value, force) at 0x400e0ddc, through this mod's site,
as the MIDI tracks' task does. Live input goes in at MIDI IN. Every byte that
leaves on DIN (din_send 0x400028c6, stock's and this mod's) is checked against
a model of the expected sums.

    set -a; . toolchain/emu-mx080.env; set +a
    digiemu/.venv/bin/python scripts/emu-midi-whacky.py
"""
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dtsession  # noqa: E402

from unicorn.m68k_const import (UC_M68K_REG_A7, UC_M68K_REG_PC, UC_M68K_REG_SR,  # noqa: E402
                                UC_M68K_REG_D0, UC_M68K_REG_A0)
from emu.session import Session, run_script  # noqa: E402

import unicorn.m68k_const as K  # noqa: E402
REGS = [getattr(K, n) for n in dir(K) if n.startswith('UC_M68K_REG_') and n[12:13] in 'AD' and len(n) == 14] + [K.UC_M68K_REG_SR]

SEND, DIN_SEND, PARAMS = 0x400e0ddc, 0x400028c6, 0x421c9188
TASK_MS = int(os.environ.get('TASK_MS', '20'))  # the MIDI tracks' task sends: its next pass
AUTO = 13                       # the auto channel (0-based), as the fresh project has it

out = []                        # every message sent on DIN: bytes
fails = []
checks = [0]


def check(cond, what):
    checks[0] += 1
    if not cond:
        fails.append(what)
        print('FAIL', what)


def u32(uc, a):
    return struct.unpack('>I', bytes(uc.mem_read(a, 4)))[0]


def main():
    syx = os.environ['DT2_SYX']
    snap = os.path.abspath(os.environ['DT2_SNAPSHOT'])
    os.chdir(os.path.dirname(syx))
    io = dtsession.Midi()

    def on_machine(s):
        io.on_machine(s)

    seen = [0]
    stream = bytearray()

    def collect():
        """What left on DIN since the last call (digiemu's MIDI OUT), as messages."""
        for _, data in io.out[seen[0]:]:
            stream.extend(data)
        seen[0] = len(io.out)
        i = 0
        while i < len(stream):
            st = stream[i]
            if st >= 0xF8 or st < 0x80:     # clock, realtime, or a stray data byte
                if st < 0x80:
                    out.append(bytes([st]))  # reported as invalid below
                i += 1
                continue
            ln = 2 if (st & 0xE0) == 0xC0 or (st & 0xF0) == 0xD0 else 3
            if i + ln > len(stream):
                break
            out.append(bytes(stream[i:i + ln]))
            i += ln
        del stream[:i]

    s = Session(snap, syx, audio=False, on_machine=on_machine)
    uc = s.m.uc

    def steps(text):
        for st in dtsession.parse(text):
            if st[0] == 'midi':
                io.send(s, st[1])
            else:
                run_script(s, [st])
        collect()

    def midi(*b):
        io.send(s, bytes(b))
        s.run_ms(TASK_MS)
        collect()

    def call_send(track, ch, oid, value, force=0):
        """The firmware's send(), as the MIDI tracks' task calls it; returns
        the messages it caused on DIN at once (this mod's) - stock's own go
        into its CC buffer and leave on the task's next pass."""
        saved = {r: uc.reg_read(r) for r in REGS}
        sp = uc.reg_read(UC_M68K_REG_A7) - 0x400
        magic = 0x40000400
        args = [magic, track, ch, 21 + oid, value & 0xFFFFFFFF, force]
        for i, a in enumerate(args):
            uc.mem_write(sp + 4 * i, struct.pack('>I', a & 0xFFFFFFFF))
        uc.reg_write(UC_M68K_REG_A7, sp)
        uc.reg_write(UC_M68K_REG_SR, 0x2700)
        before = len(out)
        stepper = getattr(s.m, '_fast_stepper_obj', None)   # digiemu bounds each run: lift it for this call
        left = getattr(stepper, 'left', None)
        if stepper is not None:
            stepper.left = 1 << 40
        try:
            uc.emu_start(SEND, magic, count=200000)
        finally:
            if stepper is not None:
                stepper.left = left
        pc = uc.reg_read(UC_M68K_REG_PC)
        check(pc == magic, 'send() returned (pc %08x)' % pc)
        for r, v in saved.items():
            try:
                uc.reg_write(r, v)
            except Exception:
                pass
        s.run_ms(TASK_MS)                       # the task's pass, then the DMA to MIDI OUT
        collect()
        return out[before:]

    try:
        s.run_ms(200)
        mem = bytes(uc.mem_read(0x47be0000, 0x20000))
        audio = 0x47be0000 + mem.find(bytes([2, 0, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0]) * 8)
        midi_cfg = 0x47be0000 + mem.find(bytes([0, 1, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0]) * 8)
        print('settings: audio %08x, MIDI %08x' % (audio, midi_cfg))

        def cfg(m, thru=1, mw=0, mwa=0, at=0, ata=0, vel=0, vela=0, sens=100):
            uc.mem_write(midi_cfg + 12 * m, struct.pack('>hbbbbhhh', thru, mw, at, vel, sens, mwa, ata, vela))

        def sel(m, k, cc):
            uc.mem_write(PARAMS + 106 * m + 2 * (33 + k), struct.pack('>b', cc))

        # MIDI track A selected, CHAN on (channel 1)
        steps('press TRK 100\ntap 9\nrelease TRK 100\nwait 200\npress FUNC 100\ntap A\nrelease FUNC 100\nwait 300\n')

        def cc_vals(msgs, cc):
            return [m[2] for m in msgs if len(m) == 3 and (m[0] & 0xF0) == 0xB0 and m[1] == cc]

        # 1. VAL1 on CC 1 (SEL1 = 1), wheel -> VAL1 +16, THRU on; the track's LFO on both MW and VAL1.
        print('--- 1. VAL1 = CC 1 merged with the mod wheel, LFOs on both')
        sel(0, 0, 1)
        cfg(0, thru=1, mw=5, mwa=16 * 256)
        call_send(0, 0, 4, 40 << 8)             # VAL1 40
        call_send(0, 0, 2, 20 << 8)             # MW 20 (latest)
        n0 = len(out)
        midi(0xB0 | AUTO, 1, 64)
        got = cc_vals(out[n0:], 1)
        check(got == [20 + 64 + 16], 'wheel 64: one CC 1 = MW 20 + 64 + 16: %s' % got)
        latest = 20
        wheel = 64
        heard = [20 + 64 + 16]
        rng = random.Random(7)
        for i in range(120):
            if i % 3 == 2:
                wheel = rng.randrange(128)
                n0 = len(out)
                midi(0xB0 | AUTO, 1, wheel)
            else:
                oid = 2 if i % 3 == 0 else 4
                latest = rng.randrange(128)
                n0 = len(out)
                got = call_send(0, 0, oid, latest << 8)
                got = cc_vals(got, 1)
            exp = min(127, latest + wheel + (wheel * 16 * 256 // 64 + 128) // 256)
            got = cc_vals(out[n0:], 1)
            if exp == heard[0]:                 # unchanged: not sent again (0.9, as stock)
                check(got == [], 'step %d: CC 1 %s, expected nothing (still %d)' % (i, got, exp))
            else:
                check(got == [exp], 'step %d: CC 1 %s, expected [%d]' % (i, got, exp))
            heard[0] = exp
        s.run_ms(50)
        collect()                               # the MIDI task's pass: stock's buffer, if anything was left in it
        stray = [m for m in out[n0:] if len(m) == 3 and m[0] == 0xB0 and m[1] == 1 and m[2] != exp]
        check(not stray, 'no stray CC 1 from stock while merged: %s' % stray)

        print('--- 2. track change: CC 1 back to the latest own value, then stock')
        n0 = len(out)
        steps('press TRK 100\ntap 1\nrelease TRK 100\nwait 200\n')
        got = cc_vals(out[n0:], 1)
        check(got[-1:] == [latest], 'released to %d: %s' % (latest, got))
        steps('press TRK 100\ntap 9\nrelease TRK 100\nwait 200\n')
        n0 = len(out)
        got = call_send(0, 0, 2, 33 << 8)
        check(cc_vals(got, 1) == [], 'MW send not taken over any more (stock buffer): %s' % got)

        print('--- 3. velocity -> BC: the CC before its note; V.SNS 50 on the note')
        cfg(0, thru=1, vel=4, vela=64 * 256, sens=50)
        sel(0, 0, 74)
        n0 = len(out)
        midi(0x90 | AUTO, 60, 20)
        s.run_ms(30)
        collect()
        seq = [m for m in out[n0:] if m[0] in (0xB0, 0x90)]
        check(len(seq) >= 2 and seq[0][:2] == b'\xb0\x02' and seq[0][2] == 20 and seq[1][0] == 0x90 and seq[1][2] == 74,
              'CC 2 = 20, then the note at 74: %s' % [m.hex() for m in seq])
        midi(0x80 | AUTO, 60, 0)

        print('--- 4. pressure -> pitch bend -64 with the bend down: capped at 0')
        cfg(0, thru=1, at=1, ata=-64 * 256)
        n0 = len(out)
        midi(0xE0 | AUTO, 0, 0)
        midi(0xD0 | AUTO, 127)
        pbs = [m[1] | (m[2] << 7) for m in out[n0:] if m[0] == 0xE0]
        check(pbs and pbs[-1] == 0, 'PB capped at 0: %s' % pbs)
        midi(0xE0 | AUTO, 0, 64)
        midi(0xD0 | AUTO, 0)

        print('--- 5. two slots on one CC, a slot on CC 2 with BC, SEL off, wheel on wheel')
        sel(0, 2, 30)
        sel(0, 5, 30)
        sel(0, 1, 2)
        sel(0, 3, -1)
        cfg(0, thru=1, mw=3, mwa=-64 * 256, at=7, ata=64 * 256, vel=8, vela=64 * 256)
        n0 = len(out)
        call_send(0, 0, 6, 5 << 8)              # VAL3 5
        call_send(0, 0, 9, 9 << 8)              # VAL6 9 (latest on CC 30)
        midi(0xD0 | AUTO, 20)
        check(cc_vals(out[n0:], 30)[-1:] == [29], 'CC 30 = VAL6 9 + 20: %s' % cc_vals(out[n0:], 30))
        n0 = len(out)
        midi(0xB0 | AUTO, 1, 100)
        check(cc_vals(out[n0:], 1)[-1:] == [33], 'MW: own 33 (sent in 2.) + 100 - 100 = 33: %s' % cc_vals(out[n0:], 1))
        n0 = len(out)
        midi(0xB0 | AUTO, 2, 40)
        check(cc_vals(out[n0:], 2)[-1:] == [40], 'BC thru 40 (VAL2 own 0): %s' % cc_vals(out[n0:], 2))
        midi(0x90 | AUTO, 61, 127)               # velocity -> VAL4, which has no CC number
        midi(0x80 | AUTO, 61, 0)

        print('--- 6. 600 random events: inputs, own sends, notes, settings, SEL numbers, track changes')
        rng = random.Random(1234)
        for i in range(600):
            r = rng.randrange(10)
            if r == 0:
                v = rng.randrange(16384)
                midi(0xE0 | AUTO, v & 127, v >> 7)
            elif r == 1:
                midi(0xD0 | AUTO, rng.randrange(128))
            elif r == 2:
                midi(0xB0 | AUTO, rng.choice((1, 2)), rng.randrange(128))
            elif r in (3, 4):
                oid = rng.randrange(12)
                call_send(0, 0, oid, rng.randrange(16384) if oid == 0 else rng.randrange(128) << 8)
            elif r == 5:
                n = rng.randrange(128)
                midi(0x90 | AUTO, n, 1 + rng.randrange(127))
                midi(0x80 | AUTO, n, 0)
            elif r == 6:
                cfg(0, thru=rng.randrange(2), mw=rng.randrange(13), mwa=rng.randrange(-16384, 16385),
                    at=rng.randrange(13), ata=rng.randrange(-16384, 16385),
                    vel=rng.randrange(13), vela=rng.randrange(-16384, 16385), sens=rng.randrange(101))
            elif r == 7:
                sel(0, rng.randrange(8), rng.choice((1, 2, -1, rng.randrange(128))))
            elif r == 8 and rng.randrange(4) == 0:
                t = rng.choice((1, 2, 9))
                steps('press TRK 100\ntap %d\nrelease TRK 100\nwait 100\n' % t)
                steps('press TRK 100\ntap 9\nrelease TRK 100\nwait 100\n')
            else:
                s.run_ms(20)
                collect()
        s.run_ms(200)
        collect()
        bad = [m.hex() for m in out if m[0] < 0x80 or any(x > 127 for x in m[1:])]
        check(not bad, 'every message valid (%d sent): %s' % (len(out), bad[:5]))
        n0 = len(out)
        steps('wait 300\n')
        check(True, 'the UI still runs')
    finally:
        s.close()
    print('%d checks, %d failures; %d DIN messages' % (checks[0], len(fails), len(out)))
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
