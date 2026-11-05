#!/usr/bin/env python3
"""Which of the render's 53 parameter words per track are read, and when
(research, emulator only). Phases of a render block on OS 1.54:

  build  from 0x40077e9a (the words are built) to the LFO stage call
  lfo    inside the LFO stage, jsr 0x400ed766 at 0x40077eac
  after  from 0x40077eea (MIDI Expression's hook point) to the block's end

Plays notes on the active track (MIDI note on the auto channel) so voices
run. Prints, per slot of track --track, the phases it was read in.

    digiemu/.venv/bin/python scripts/probe-reads.py --track 0 --ms 400
"""
import argparse
import collections
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))   # dtsession.py is one folder up
import dtsession  # noqa: E402

from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ  # noqa: E402
from unicorn.m68k_const import UC_M68K_REG_A7, UC_M68K_REG_D0  # noqa: E402
from emu.session import Session  # noqa: E402

BUILD, LFO_CALL, LFO_RET, AFTER = 0x40077ea0, 0x40077eac, 0x40077eb2, 0x40077eea


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--track', type=int, default=0)
    ap.add_argument('--ms', type=int, default=400)
    ap.add_argument('--auto', type=int, default=14, help='AUTO CHANNEL 1-16')
    args = ap.parse_args()
    syx = os.environ['DT2_SYX']
    snap = os.path.abspath(os.environ.get('DT2_SNAPSHOT') or os.path.join(
        os.environ['DT2_SNAPSHOTS'], 'Digitakt_OS1.54', 'gui.snap'))
    os.chdir(os.path.dirname(syx))
    io = dtsession.Midi()
    st = {'phase': None, 'words': None, 'blocks': 0}
    seen = collections.defaultdict(set)

    def on_machine(s):
        io.on_machine(s)
        uc = s.m.uc

        def code(uc, address, size, data):
            if address == BUILD:
                st['words'] = uc.reg_read(UC_M68K_REG_D0)
                st['phase'] = 'build'
                st['blocks'] += 1
            elif address == LFO_CALL:
                st['phase'] = 'lfo'
            elif address == LFO_RET:
                st['phase'] = 'build'
            elif address == AFTER:
                st['phase'] = 'after'
        for a in (BUILD, LFO_CALL, LFO_RET, AFTER):
            uc.hook_add(UC_HOOK_CODE, code, begin=a, end=a)

        def read(uc, access, address, size, value, data):
            w = st['words']
            if not w or not st['phase']:
                return
            base = w + 18 + 106 * args.track
            if base <= address < base + 106:
                slot = (address - base) // 2
                seen[slot].add(st['phase'])
        uc.hook_add(UC_HOOK_MEM_READ, read, begin=0x80000000, end=0x8000ffff)

    s = Session(snap, syx, audio=True, on_machine=on_machine)
    try:
        s.run_ms(50)
        ch = args.auto - 1
        for i in range(max(1, args.ms // 100)):
            io.send(s, bytes([0x90 | ch, 60, 100]))
            s.run_ms(60)
            io.send(s, bytes([0x80 | ch, 60, 0]))
            s.run_ms(20)
        names = ['-', 'L1.SPD', 'L1.MUL', 'L1.FADE', 'L1.DEST', 'L1.WAVE', 'L1.PHAS', 'L1.MODE', 'L1.DEP',
                 'L2.SPD', 'L2.MUL', 'L2.FADE', 'L2.DEST', 'L2.WAVE', 'L2.PHAS', 'L2.MODE', 'L2.DEP',
                 'SRC.A', 'SRC.B', 'SRC.C', 'SRC.D', 'SRC.E', 'SRC.F', 'SRC.G', 'SRC.H',
                 'FLT.TYPE', 'FLT.FREQ', 'FLT.RESO', 'FLT.ENV', 'FLT.ATK', 'FLT.DEC', 'FLT.SUS', 'FLT.REL',
                 'FLT2.A', 'FLT2.B', 'FLT2.C', 'FLT2.D', 'FLT2.E',
                 'AMP.ATK', 'AMP.HOLD', 'AMP.DEC', 'AMP.OVER', 'AMP.DEL', 'AMP.REV', 'AMP.PAN', 'AMP.VOL']
        print('blocks: %d, words at %s' % (st['blocks'], hex(st['words'] or 0)))
        for slot in range(53):
            nm = names[slot] if slot < len(names) else 'slot%d' % slot
            ph = ','.join(sorted(seen.get(slot, ()))) or 'never'
            print('%2d %-9s %s' % (slot, nm, ph))
    finally:
        s.close()


if __name__ == '__main__':
    main()
