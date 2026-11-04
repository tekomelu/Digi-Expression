#!/usr/bin/env python3
"""Log calls to chosen firmware functions while a step script runs.

Research tool, emulator only. Each hit prints the emulated time, the
function's name, the return address (who called it), the stack pointer
(which task's stack) and the first stack arguments. MIDI OUT bytes are
logged in the same stream, so the order of events can be read off.

    set -a; . toolchain/emu-dt154.env; set +a
    digiemu/.venv/bin/python scripts/trace-calls.py \
        --snapshot "$DT2_SNAPSHOTS/Digitakt_OS1.54/trackA-chan1.snap" \
        -f append=0x400e0dac -f queue_send=0x40001b7a:ret \
        -e "midi 9d 3c 64; wait 50"

`name=addr` logs a function; `name=addr:bytes` also dumps the bytes its
2nd argument points to with the 1st as length (append(len, bytes)).
Prints addresses and the bytes of MIDI messages only.
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


class Calls:
    def __init__(self, funcs, limit):
        self.io = dtsession.Midi()
        self.funcs = funcs          # addr -> (name, mode)
        self.limit = limit
        self.lines = []
        self.counts = {}
        self.s = None
        self.io._sink_orig = self.io._sink
        self.io._sink = self._out
        self.io.midi_out.sink = self._out

    def _out(self, data):
        self.io._sink_orig(data)
        ms = self.s.ms if self.s else 0
        chain = ''
        if self.s is not None:
            from unicorn.m68k_const import UC_M68K_REG_PC
            uc = self.s.m.uc
            sp = uc.reg_read(UC_M68K_REG_A7)
            words = struct.unpack('>64I', bytes(uc.mem_read(sp, 256)))
            code = [w for w in words if 0x40000400 <= w < 0x401b0000 and not w & 1]
            chain = '  pc %#010x sp %#010x  stack code ptrs %s' % (
                uc.reg_read(UC_M68K_REG_PC), sp, ' '.join('%08x' % w for w in code[:12]))
        self.lines.append('%9.2f  MIDI OUT  %s%s' % (ms, data.hex(' '), chain))

    def on_machine(self, s):
        self.s = s
        self.io.on_machine(s)
        for addr in self.funcs:
            s.m.uc.hook_add(UC_HOOK_CODE, self._hit, begin=addr, end=addr)

    def _hit(self, uc, address, size, data):
        name, mode = self.funcs[address]
        n = self.counts.get(name, 0) + 1
        self.counts[name] = n
        if n > self.limit:
            return
        sp = uc.reg_read(UC_M68K_REG_A7)
        ret, a1, a2, a3, a4 = struct.unpack('>5I', bytes(uc.mem_read(sp, 20)))
        extra = ''
        if mode == 'bytes' and 0 < a1 <= 16:
            try:
                extra = '  [%s]' % bytes(uc.mem_read(a2, a1)).hex(' ')
            except Exception:                               # noqa: BLE001
                pass
        ms = self.s.ms if self.s else 0
        self.lines.append('%9.2f  %-12s from %#010x  sp %#010x  args %08x %08x %08x %08x%s'
                          % (ms, name, ret, sp, a1, a2, a3, a4, extra))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--snapshot', required=True)
    ap.add_argument('-f', '--func', action='append', default=[],
                    help='name=0xaddr or name=0xaddr:bytes')
    ap.add_argument('-e', '--steps', required=True)
    ap.add_argument('--limit', type=int, default=400, help='hits logged per function')
    args = ap.parse_args()

    funcs = {}
    for spec in args.func:
        name, _, rest = spec.partition('=')
        addr, _, mode = rest.partition(':')
        funcs[int(addr, 0)] = (name, mode)

    syx = os.environ['DT2_SYX']
    snapshot = os.path.abspath(args.snapshot)
    os.chdir(os.path.dirname(syx))
    c = Calls(funcs, args.limit)
    s = Session(snapshot, syx, audio=False, on_machine=c.on_machine)
    try:
        s.run_ms(100)
        c.lines.append('%9.2f  --- start' % s.ms)
        for step in dtsession.parse(args.steps.replace(';', '\n')):
            if step[0] == 'midi':
                c.lines.append('%9.2f  MIDI IN   %s' % (s.ms, step[1].hex(' ')))
                c.io.send(s, step[1])
            elif step[0] == 'mark':
                c.lines.append('%9.2f  --- %s' % (s.ms, step[1]))
            else:
                run_script(s, [step])
        print('\n'.join(c.lines))
        print('counts:', c.counts)
        if s.halted:
            print('HALTED', s.halted)
    finally:
        s.close()


if __name__ == '__main__':
    main()
