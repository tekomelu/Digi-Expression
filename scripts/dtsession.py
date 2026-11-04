#!/usr/bin/env python3
"""Drive the emulated Digitakt from a step script: keys, MIDI in, screen
captures and a log of everything sent to MIDI OUT.

Uses digiemu's Session (emu/session.py): the same machine the app builds,
stepped in emulated time, with key codes measured for the mk1 (the device
file's [panel.labels], not the firmware's panel-test name table, whose order
is not the runtime key map on the mk1). MIDI goes through digiemu's
emu/midi.py: bytes into the DIN input's DMA ring and its interrupt, and
UART9's transmitter captured on the way out.

Script lines (one per line, or ';'-separated with -e):

    tap GLOBAL            # tap a key (labels: PLAY, STOP, FUNC, TRK, YES, NO, 1..16, ...)
    press FUNC            # hold a key
    release FUNC
    turn A 3              # turn encoder A three detents
    wait 500              # run 500 emulated ms
    snap settings         # save the current screen as <out>/NN-settings.png
    midi 9d 3c 64         # send these bytes into MIDI IN (hex), then run 20 ms
    mark bend             # write a marker into the MIDI OUT log

    source env.sh; set -a; . toolchain/emu-dt154.env; set +a
    digiemu/.venv/bin/python scripts/dtsession.py steps.txt captures/run1
    digiemu/.venv/bin/python scripts/dtsession.py -e "tap GLOBAL; snap settings" captures/run1

Writes <out>/midi-out.txt: one line per chunk of bytes the firmware sent,
with the emulated time. --save writes the session's state as a snapshot,
so a long setup is done once and later runs start from it.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DIGIEMU = os.path.abspath(os.environ.get('DIGIEMU_DIR', os.path.join(HERE, '..', 'digiemu')))
sys.path.insert(0, DIGIEMU)

from unicorn.m68k_const import UC_M68K_REG_PC  # noqa: E402

from emu import fwcompare, midi, panel  # noqa: E402
from emu.session import Session, run_script  # noqa: E402

MIDI_SETTLE_MS = 20


def parse(text):
    """-> [('midi', bytes) | ('mark', label) | fwcompare step]."""
    steps = []
    for raw in text.splitlines():
        line = raw.split(' # ')[0].strip()
        if not line or line.startswith('#'):
            continue
        verb, _, rest = line.partition(' ')
        if verb.lower() == 'midi':
            steps.append(('midi', bytes(int(t, 16) for t in rest.split())))
        elif verb.lower() == 'flood':
            # flood <status hex> <count> <interval ms>: pitch bend (Ex) sweeping
            # 0..16383 and back, aftertouch (Dx) or CC 1 (Bx) sweeping 0..127
            tok = rest.split()
            steps.append(('flood', int(tok[0], 16), int(tok[1]), float(tok[2])))
        elif verb.lower() == 'mark':
            steps.append(('mark', rest.strip()))
        elif verb.lower() == 'poke':
            # poke <addr hex> <value>: write a 32-bit word (test setup only)
            tok = rest.split()
            steps.append(('poke', int(tok[0], 16), int(tok[1], 0)))
        else:
            steps.extend(fwcompare.parse_script(line))
    return steps


class Midi:
    def __init__(self):
        self.out = []                    # (ms, bytes) the firmware sent
        self.log = []                    # (ms, text) in order: out bytes and marks
        self.session = None
        self.midi_in = midi.MidiIn()     # not attached: delivered between chunks
        self.midi_out = midi.MidiOut(self._sink)

    def _sink(self, data):
        ms = self.session.ms if self.session else 0.0
        self.out.append((ms, bytes(data)))
        self.log.append((ms, 'OUT ' + data.hex(' ')))

    attached = os.environ.get('DTSESSION_MIDI_ATTACH') == '1'

    def on_machine(self, s):
        self.session = s
        self.midi_out.install(s.m)
        self.midi_out.attach(s.m, s.ips)
        extra = (self.midi_out,)
        if self.attached:                # as digiemu's app: an event source on emulated time
            self.midi_in.attach(s.m, s.ips)
            extra = (self.midi_in, self.midi_out)
        s.sources = tuple(s.sources) + extra

    def send(self, s, data):
        self.log.append((s.ms, 'IN  ' + data.hex(' ')))
        self.midi_in.put(data)
        if self.attached:
            s.run_ms(MIDI_SETTLE_MS)
            return
        for _ in range(200):             # the vector may be masked for a moment
            if self.midi_in.deliver(s.m):
                s.pc = s.m.uc.reg_read(UC_M68K_REG_PC)
            if not self.midi_in._pending:
                break
            s.run_ms(1)
        else:
            self.log.append((s.ms, 'IN  (not delivered: %d bytes left)'
                             % len(self.midi_in._pending)))
        s.run_ms(MIDI_SETTLE_MS)

    def flood(self, s, status, count, interval):
        """Messages every `interval` ms, delivered without the settle time."""
        kind = status & 0xF0
        self.log.append((s.ms, 'IN  flood %02x x%d every %.2f ms' % (status, count, interval)))
        for i in range(count):
            phase = i % 64
            v = phase if phase < 32 else 63 - phase          # triangle 0..31..0
            if kind == 0xE0:
                value = v * 528                                # 0..16368
                data = bytes([status, value & 0x7F, value >> 7])
            elif kind == 0xD0:
                data = bytes([status, v * 4])
            else:
                data = bytes([status, 1, v * 4])
            self.midi_in.put(data)
            if self.midi_in.deliver(s.m):
                s.pc = s.m.uc.reg_read(UC_M68K_REG_PC)
            s.run_ms(interval)
        for _ in range(200):
            if not self.midi_in._pending:
                break
            if self.midi_in.deliver(s.m):
                s.pc = s.m.uc.reg_read(UC_M68K_REG_PC)
            s.run_ms(1)
        self.log.append((s.ms, 'IN  flood done'))


def save_session(s, path):
    """Save the session as a snapshot, as emu/gui.py's save on exit does."""
    from emu.snapshot import save as save_snapshot
    ev, st = s.ev, s.st
    components = ev['checkpoint_components']
    if components.get('timers') is not s.pits:
        ev['claim_checkpoint_component']('timers', s.pits)
    tasks = {'%#010x' % tcb: info
             for tcb, info in st.get('task_create_hits', {}).items()}
    tasks.update(('%#010x' % tcb, {'entry': entry, 'prio': prio, 'tcb': tcb})
                 for entry, prio, tcb in ev.get('tasks', []))
    s.m.uc.reg_write(UC_M68K_REG_PC, s.pc)
    extra = {'n': st.get('n', 0), 'tasks': tasks,
             'note': 'saved by digitakt-midi-fun scripts/dtsession.py'}
    tmp = path + '.tmp'
    save_snapshot(s.m, tmp, extra=extra, components=components,
                  manifest=ev.get('checkpoint_manifest'))
    os.replace(tmp, path)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('script', nargs='?', help='step script file')
    ap.add_argument('out', help='directory for PNG captures and midi-out.txt')
    ap.add_argument('-e', '--steps', help='inline steps, separated by ";"')
    ap.add_argument('--snapshot', help='default: $DT2_SNAPSHOTS/Digitakt_OS1.54/gui.snap')
    ap.add_argument('--settle', type=int, default=300, help='ms to run before the first step')
    ap.add_argument('--audio', action='store_true', help='also run the audio path')
    ap.add_argument('--save', help='write the final state to this snapshot file')
    args = ap.parse_args(argv)

    if args.steps:
        text = args.steps.replace(';', '\n')
    elif args.script:
        text = open(args.script).read()
    else:
        ap.error('give a script file or -e')
    steps = parse(text)

    syx = os.environ['DT2_SYX']
    snapshot = args.snapshot or os.path.join(os.environ['DT2_SNAPSHOTS'], 'Digitakt_OS1.54', 'gui.snap')
    snapshot = os.path.abspath(snapshot)
    save = os.path.abspath(args.save) if args.save else None
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    os.chdir(os.path.dirname(syx))

    io = Midi()
    s = Session(snapshot, syx, audio=args.audio, on_machine=io.on_machine)
    status = 0
    try:
        s.run_ms(args.settle)
        marks = [('start', s.ms)]
        for step in steps:
            if step[0] == 'midi':
                io.send(s, step[1])
            elif step[0] == 'flood':
                io.flood(s, *step[1:])
            elif step[0] == 'mark':
                io.log.append((s.ms, '--- ' + step[1]))
            elif step[0] == 'poke':
                s.m.uc.mem_write(step[1], (step[2] & 0xFFFFFFFF).to_bytes(4, 'big'))
                io.log.append((s.ms, 'POKE %#010x = %d' % (step[1], step[2])))
            else:
                marks.extend(run_script(s, [step]))
            if s.halted:
                break
        for i, (label, ms) in enumerate(marks):
            buf = s.screen_at(ms)
            if buf is None:
                print('%-14s %8.1f ms  no frame yet' % (label, ms))
                continue
            path = os.path.join(out, '%02d-%s.png' % (i, label))
            panel.write_png(buf, path, scale=6)
            print('%-14s %8.1f ms  %4d lit  -> %s' % (label, ms, len(panel.lit(buf)), path))
        with open(os.path.join(out, 'midi-out.txt'), 'w') as fh:
            for ms, text in io.log:
                fh.write('%9.1f  %s\n' % (ms, text))
        print('MIDI: %d bytes in, %d bytes out (log: %s)'
              % (io.midi_in.delivered, io.midi_out.sent, os.path.join(out, 'midi-out.txt')))
        if s.halted:
            print('HALTED:', s.halted)
            status = 1
        elif save:
            save_session(s, save)
            print('saved', save)
    finally:
        s.close()
    return status


if __name__ == '__main__':
    sys.exit(main())
