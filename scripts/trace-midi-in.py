#!/usr/bin/env python3
"""Trace what the firmware does with each message arriving at MIDI IN.

Research tool, emulator only. For every message the MIDI parser posts to the
MIDI task's DIN queue (queue_send(DIN_QUEUE, item), the path FX Track's
TESTING.md documents for OS 1.54), it logs the code addresses that read the
message's bytes afterwards, in first-read order, so a note and a pitch bend
can be compared to see where their paths part.

Prints addresses only; no firmware bytes are written anywhere.

    set -a; . toolchain/emu-dt154.env; set +a
    digiemu/.venv/bin/python scripts/trace-midi-in.py \
        --snapshot "$DT2_SNAPSHOTS/Digitakt_OS1.54/trackA-chan1.snap" \
        9d3c64 ed0060 dd50 bd0140
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dtsession  # noqa: E402  (sets up digiemu's path)

from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ  # noqa: E402
from unicorn.m68k_const import UC_M68K_REG_A7  # noqa: E402

from emu.session import Session  # noqa: E402

# OS 1.54, from FX Track's fxtrack.s / TESTING.md.
QUEUE_SEND = 0x40001b7a
DIN_QUEUE = 0x421a9d7c
USB_QUEUE = 0x421a9d5c


class Tracer:
    def __init__(self):
        self.io = dtsession.Midi()
        self.posts = []          # (queue, item addr, length, data addr)
        self.watch = None        # (lo, hi) of the message being followed
        self.reads = []          # (pc, addr) first reads of it

    def on_machine(self, s):
        self.io.on_machine(s)
        uc = s.m.uc
        uc.hook_add(UC_HOOK_CODE, self._on_send, begin=QUEUE_SEND, end=QUEUE_SEND)
        uc.hook_add(UC_HOOK_MEM_READ, self._on_read)

    def _on_send(self, uc, address, size, data):
        sp = uc.reg_read(UC_M68K_REG_A7)
        queue, item = struct.unpack('>II', bytes(uc.mem_read(sp + 4, 8)))
        if queue not in (DIN_QUEUE, USB_QUEUE):
            return
        try:
            length, ptr = struct.unpack('>II', bytes(uc.mem_read(item, 8)))
        except Exception:                                   # noqa: BLE001
            length, ptr = 0, 0
        self.posts.append((queue, item, length, ptr))
        if 0 < length <= 16:
            self.watch = (ptr, ptr + length)

    def _on_read(self, uc, access, address, size, value, data):
        w = self.watch
        if w and w[0] <= address < w[1]:
            pc = uc.reg_read(0) if False else None
            from unicorn.m68k_const import UC_M68K_REG_PC
            pc = uc.reg_read(UC_M68K_REG_PC)
            self.reads.append((pc, address - w[0]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('messages', nargs='+', help='hex messages, e.g. 9d3c64 ed0060')
    ap.add_argument('--snapshot', required=True)
    ap.add_argument('--ms', type=int, default=40, help='ms to follow each message')
    args = ap.parse_args()

    syx = os.environ['DT2_SYX']
    snapshot = os.path.abspath(args.snapshot)
    os.chdir(os.path.dirname(syx))
    t = Tracer()
    s = Session(snapshot, syx, audio=False, on_machine=t.on_machine)
    try:
        s.run_ms(200)
        for msg in args.messages:
            data = bytes.fromhex(msg)
            t.posts.clear()
            t.reads.clear()
            t.watch = None
            out_before = len(t.io.out)
            t.io.midi_in.put(data)
            for _ in range(200):
                if t.io.midi_in.deliver(s.m):
                    from unicorn.m68k_const import UC_M68K_REG_PC
                    s.pc = s.m.uc.reg_read(UC_M68K_REG_PC)
                if not t.io.midi_in._pending:
                    break
                s.run_ms(1)
            s.run_ms(args.ms)
            t.watch = None
            print('=== IN %s' % data.hex(' '))
            for q, item, length, ptr in t.posts:
                print('  posted to %s queue: item %#010x, %d bytes at %#010x'
                      % ('DIN' if q == DIN_QUEUE else 'USB', item, length, ptr))
            seen = []
            for pc, off in t.reads:
                key = (pc, off)
                if key not in seen:
                    seen.append(key)
            for pc, off in seen[:60]:
                print('  read byte %d at pc %#010x' % (off, pc))
            if len(seen) > 60:
                print('  ... %d more' % (len(seen) - 60))
            out = t.io.out[out_before:]
            print('  MIDI OUT: %s' % (' | '.join(d.hex(' ') for _, d in out) or 'nothing'))
    finally:
        s.close()


if __name__ == '__main__':
    main()
