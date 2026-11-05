#!/usr/bin/env python3
"""Who sends what out of the MIDI tracks' output (research): every call of
append(len, bytes) 0x400e0dac and midi_out(len, bytes) 0x400e0992 on OS
1.54, with the bytes and the return addresses up the stack.

    digiemu/.venv/bin/python scripts/probe-out.py -e "press TRK 100; tap 9; ..."
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dtsession  # noqa: E402

from unicorn import UC_HOOK_CODE  # noqa: E402
from unicorn.m68k_const import UC_M68K_REG_A7, UC_M68K_REG_A6  # noqa: E402
from emu.session import Session, run_script  # noqa: E402

FUNCS = {0x400e0dac: 'append', 0x400e0992: 'midi_out'}


def u32(uc, a):
    return struct.unpack('>I', bytes(uc.mem_read(a, 4)))[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-e', '--steps', required=True)
    args = ap.parse_args()
    syx = os.environ['DT2_SYX']
    snap = os.path.abspath(os.environ.get('DT2_SNAPSHOT') or os.path.join(
        os.environ['DT2_SNAPSHOTS'], 'Digitakt_OS1.54', 'gui.snap'))
    os.chdir(os.path.dirname(syx))
    io = dtsession.Midi()

    def on_machine(s):
        io.on_machine(s)
        uc = s.m.uc

        def hit(uc, address, size, data):
            sp = uc.reg_read(UC_M68K_REG_A7)
            n, p = u32(uc, sp + 4), u32(uc, sp + 8)
            if not 0 < n < 16:
                return
            b = bytes(uc.mem_read(p, n))
            if b[0] == 0xf8:
                return
            rets = [u32(uc, sp + 4 * i) for i in range(0, 40)]
            code = [r for r in rets if 0x40000000 <= r < 0x40200000]
            print('%-8s %s   ret %s' % (FUNCS[address], b.hex(' '), ' '.join('%08x' % r for r in code[:6])))
        for a in FUNCS:
            uc.hook_add(UC_HOOK_CODE, hit, begin=a, end=a)

    s = Session(snap, syx, audio=False, on_machine=on_machine)
    try:
        s.run_ms(100)
        for step in dtsession.parse(args.steps.replace(';', '\n')):
            if step[0] == 'midi':
                io.send(s, step[1])
            elif step[0] == 'mark':
                print('---', step[1])
            else:
                run_script(s, [step])
    finally:
        s.close()


if __name__ == '__main__':
    main()
