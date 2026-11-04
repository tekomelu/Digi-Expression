#!/usr/bin/env python3
"""Find the TRIG page's view object and how to reach it from the UI's
"brain", in the emulator. Research tool.

Hooks given code addresses; at each hit prints the stack's first words and
the address registers, once per (address, first argument). After the
steps, searches RAM for 32-bit words equal to each view seen (who holds a
pointer to it) and prints the view's first 0xa0 bytes.

    set -a; . toolchain/emu-dt154.env; set +a
    digiemu/.venv/bin/python scripts/probe-view.py --hook 0x40033076 --hook 0x400368ba \
        -e "tap TRIG; wait 300; tap TRIG; wait 300"
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dtsession  # noqa: E402

from unicorn import UC_HOOK_CODE  # noqa: E402
from unicorn.m68k_const import (UC_M68K_REG_A0, UC_M68K_REG_A1, UC_M68K_REG_A2,  # noqa: E402
                                UC_M68K_REG_A3, UC_M68K_REG_A4, UC_M68K_REG_A5,
                                UC_M68K_REG_A6, UC_M68K_REG_A7)

from emu.session import Session, run_script  # noqa: E402

AREGS = [UC_M68K_REG_A0, UC_M68K_REG_A1, UC_M68K_REG_A2, UC_M68K_REG_A3,
         UC_M68K_REG_A4, UC_M68K_REG_A5, UC_M68K_REG_A6]
RAM = [(b, 0x100000) for b in range(0x40000000, 0x48000000, 0x100000)] + [(0x80000000, 0x10000)]


def u32(uc, a):
    return struct.unpack('>I', bytes(uc.mem_read(a, 4)))[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--snapshot')
    ap.add_argument('--hook', action='append', default=[])
    ap.add_argument('--find', action='append', default=[], help='also search RAM for this word')
    ap.add_argument('--dump', action='append', default=[], help='addr:len to dump at the end')
    ap.add_argument('--magic', help='print the 8 longs at each place this long is found (0x47be0000..)')
    ap.add_argument('-e', '--steps', required=True)
    args = ap.parse_args()

    syx = os.environ['DT2_SYX']
    snapshot = os.path.abspath(args.snapshot or os.path.join(
        os.environ['DT2_SNAPSHOTS'], 'Digitakt_OS1.54', 'gui.snap'))
    os.chdir(os.path.dirname(syx))
    io = dtsession.Midi()
    hooks = [int(h, 16) for h in args.hook]
    seen = {}
    views = set()

    def on_machine(s):
        io.on_machine(s)

        def hit(uc, address, size, data):
            sp = uc.reg_read(UC_M68K_REG_A7)
            stack = [u32(uc, sp + 4 * i) for i in range(12)]
            key = (address, stack[1])
            views.add(stack[1])
            if key in seen:
                seen[key] += 1
                return
            seen[key] = 1
            regs = ' '.join('a%d=%08x' % (i, uc.reg_read(r)) for i, r in enumerate(AREGS))
            vt = u32(uc, stack[1]) if 0x40000000 <= stack[1] < 0x48000000 else 0
            print('HIT %08x  arg1=%08x (vtable %08x)  stack %s\n    %s'
                  % (address, stack[1], vt, ' '.join('%08x' % w for w in stack), regs))
        for h in hooks:
            s.m.uc.hook_add(UC_HOOK_CODE, hit, begin=h, end=h)

    s = Session(snapshot, syx, audio=False, on_machine=on_machine)
    try:
        s.run_ms(300)
        for step in dtsession.parse(args.steps.replace(';', '\n')):
            if step[0] == 'midi':
                io.send(s, step[1])
            elif step[0] == 'mark':
                print('--- %s  (%.0f ms)' % (step[1], s.ms))
            elif step[0] == 'poke':
                s.m.uc.mem_write(step[1], (step[2] & 0xFFFFFFFF).to_bytes(4, 'big'))
            else:
                run_script(s, [step])
        print('hits:', {('%08x' % a, '%08x' % v): n for (a, v), n in seen.items()})
        uc = s.m.uc
        targets = {v for v in views if 0x40000000 <= v < 0x48000000}
        targets |= {int(f, 16) for f in args.find}
        for base, size in RAM:
            try:
                mem = bytes(uc.mem_read(base, size))
            except Exception:
                continue
            for t in sorted(targets):
                pat = struct.pack('>I', t)
                i = mem.find(pat)
                n = 0
                while i >= 0 and n < 8:
                    if i % 2 == 0:
                        print('REF %08x at %08x' % (t, base + i))
                        n += 1
                    i = mem.find(pat, i + 1)
        for t in sorted(v for v in views if 0x40000000 <= v < 0x48000000):
            b = bytes(uc.mem_read(t, 0xa0))
            print('VIEW %08x:' % t)
            for o in range(0, 0xa0, 16):
                print('  +%02x %s' % (o, b[o:o + 16].hex(' ')))
        brain = u32(uc, 0x421fac10)
        ctrl = brain + 0x40
        first, last, focus = u32(uc, ctrl + 0x14), u32(uc, ctrl + 0x18), u32(uc, ctrl + 0x1c)
        frame = u32(uc, first + 8)
        page = u32(uc, frame + 0xb4)
        info = ''
        if 0x40000000 <= page < 0x48000000:
            b, e, i = u32(uc, page + 124), u32(uc, page + 128), u32(uc, page + 144)
            kind = u32(uc, b + 4 * i) if 0x40000000 <= b < 0x48000000 and b < e else -1
            info = 'vtable %08x key %d kind %d' % (u32(uc, page), u32(uc, page + 0x88), kind)
        print('CHAIN brain %08x layers first %08x last %08x focus %08x%s\n  frame %08x (vtable %08x) page %08x %s'
              % (brain, first, last, focus, '' if first == focus else ' (OVERLAY)', frame, u32(uc, frame), page, info))
        if args.magic:
            pat = struct.pack('>I', int(args.magic, 16))
            mem = bytes(uc.mem_read(0x47be0000, 0x20000))
            i = mem.find(pat)
            while i >= 0:
                if i % 4 == 0:
                    print('MAGIC at %08x: %s' % (0x47be0000 + i, ' '.join(
                        str(struct.unpack('>i', mem[i + 4 * j:i + 4 * j + 4])[0]) for j in range(8))))
                i = mem.find(pat, i + 1)
        for d in args.dump:
            a, n = d.split(':')
            a, n = int(a, 16), int(n, 0)
            b = bytes(uc.mem_read(a, n))
            print('DUMP %08x:' % a)
            for o in range(0, n, 16):
                print('  +%03x %s' % (o, b[o:o + 16].hex(' ')))
    finally:
        s.close()


if __name__ == '__main__':
    main()
