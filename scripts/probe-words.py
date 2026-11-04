#!/usr/bin/env python3
"""Read the audio render's parameter words in the emulator, after MIDI
Expression's render hook, while MIDI is sent.

Research and test tool, emulator only. A code hook at 0x400ece8e (OS 1.54:
the call the hook hands over to) reads one slot of one track each block.
Prints the last value seen after each step, and the distinct values seen.

    set -a; . toolchain/emu-mx030.env; set +a
    digiemu/.venv/bin/python scripts/probe-words.py --snapshot S --track 0 --slot 17 \
        -e "mark centre; wait 50; mark bend; midi e0 7f 7f; wait 50"
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dtsession  # noqa: E402

from unicorn import UC_HOOK_CODE  # noqa: E402
from unicorn.m68k_const import UC_M68K_REG_A7  # noqa: E402

from emu.session import Session, run_script  # noqa: E402

NEXT = 0x400ece8e


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--snapshot', required=True)
    ap.add_argument('--track', type=int, default=0)
    ap.add_argument('--slot', type=int, action='append', required=True)
    ap.add_argument('-e', '--steps', required=True)
    args = ap.parse_args()

    syx = os.environ['DT2_SYX']
    snapshot = os.path.abspath(args.snapshot)
    os.chdir(os.path.dirname(syx))
    io = dtsession.Midi()
    seen = {s: [] for s in args.slot}
    blocks = [0]

    def on_machine(s):
        io.on_machine(s)

        def hit(uc, address, size, data):
            sp = uc.reg_read(UC_M68K_REG_A7)
            words = struct.unpack('>I', bytes(uc.mem_read(sp + 4, 4)))[0]
            blocks[0] += 1
            for sl in args.slot:
                off = words + 18 + 106 * args.track + 2 * sl
                v = struct.unpack('>h', bytes(uc.mem_read(off, 2)))[0]
                seen[sl].append(v)
        s.m.uc.hook_add(UC_HOOK_CODE, hit, begin=NEXT, end=NEXT)

    s = Session(snapshot, syx, audio=True, on_machine=on_machine)
    try:
        s.run_ms(100)
        for step in dtsession.parse(args.steps.replace(';', '\n')):
            if step[0] == 'midi':
                io.send(s, step[1])
            elif step[0] == 'mark':
                print('--- %s' % step[1])
                for sl in args.slot:
                    seen[sl].clear()
                continue
            else:
                run_script(s, [step])
            for sl in args.slot:
                vals = seen[sl]
                distinct = sorted(set(vals))
                print('  slot %d: last %s, distinct %s (%d blocks)%s'
                      % (sl, vals[-1] if vals else None, distinct[:8], len(vals),
                         ' ...' if len(distinct) > 8 else ''))
        if s.halted:
            print('HALTED', s.halted)
    finally:
        s.close()


if __name__ == '__main__':
    main()
