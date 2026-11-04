#!/usr/bin/env python3
"""Calls of the firmware's encoder accelerator 0x400c06ae (OS 1.54) and what
they return, while steps run (research): state, encoder, raw x multiplier,
curve, pushed -> words; and who called it.

    digiemu/.venv/bin/python scripts/probe-acc.py -e "tap LFO; wait 300; turn H 1"
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dtsession  # noqa: E402

from unicorn import UC_HOOK_CODE  # noqa: E402
from unicorn.m68k_const import UC_M68K_REG_A7, UC_M68K_REG_D0  # noqa: E402
from emu.session import Session, run_script  # noqa: E402

ACC = 0x400c06ae


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
    pend = {}

    def on_machine(s):
        io.on_machine(s)
        uc = s.m.uc

        def hit(uc, address, size, data):
            sp = uc.reg_read(UC_M68K_REG_A7)
            ret = u32(uc, sp)
            a = [u32(uc, sp + 4 * i) for i in range(1, 6)]
            pend[ret] = a
            uc.hook_add(UC_HOOK_CODE, back, begin=ret, end=ret)

        def back(uc, address, size, data):
            if address in pend:
                a = pend.pop(address)
                raw = a[2] if a[2] < 0x80000000 else a[2] - (1 << 32)
                d0 = uc.reg_read(UC_M68K_REG_D0)
                d0 = d0 if d0 < 0x80000000 else d0 - (1 << 32)
                print('acc(state %08x, enc %d, raw %d, curve %08x, push %d) -> %d   from %08x'
                      % (a[0], a[1], raw, a[3], a[4], d0, address))
        uc.hook_add(UC_HOOK_CODE, hit, begin=ACC, end=ACC)

    s = Session(snap, syx, audio=False, on_machine=on_machine)
    try:
        s.run_ms(100)
        for step in dtsession.parse(args.steps.replace(';', '\n')):
            if step[0] == 'mark':
                print('---', step[1])
            else:
                run_script(s, [step])
    finally:
        s.close()


if __name__ == '__main__':
    main()
