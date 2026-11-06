#!/usr/bin/env python3
"""Which MIDI connection reaches the Digitakt, and which comes back?

Opens every MIDI input, then sends a note and a mod wheel on each given
channel out of each output in turn, and prints what every input heard (clock
counted, not printed). Nothing else is sent.

    ~/.venvs/dt/bin/python scripts/hwcheck.py                 # channels 9..16
    ~/.venvs/dt/bin/python scripts/hwcheck.py --out "Portti 1" --out "Digitakt"
"""
import argparse
import sys
import threading
import time

import rtmidi


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--channels', default='9,10,11,12,13,14,15,16')
    ap.add_argument('--out', action='append', help='only outputs whose name contains this (repeatable)')
    a = ap.parse_args()
    chans = [int(c) - 1 for c in a.channels.split(',')]
    ins, outs = rtmidi.MidiIn().get_ports(), rtmidi.MidiOut().get_ports()
    print('inputs: ', ins)
    print('outputs:', outs)
    heard, lock = {n: [] for n in ins}, threading.Lock()
    opened = []
    for i, n in enumerate(ins):
        mi = rtmidi.MidiIn()
        mi.ignore_types(sysex=True, timing=False, active_sense=True)
        mi.open_port(i)
        mi.set_callback(lambda ev, data=None, n=n: (lock.acquire(), heard[n].append(bytes(ev[0])), lock.release()))
        opened.append(mi)
    time.sleep(1.0)
    with lock:
        for n in ins:
            clocks = sum(1 for m in heard[n] if m[0] == 0xF8)
            other = [m.hex() for m in heard[n] if m[0] != 0xF8]
            print('\nidle 1 s, %r: %d clock, other: %s' % (n, clocks, ' '.join(other[:10]) or 'none'))
            heard[n].clear()
    for j, out in enumerate(outs):
        if a.out and not any(w.lower() in out.lower() for w in a.out):
            continue
        mo = rtmidi.MidiOut()
        mo.open_port(j)
        for ch in chans:
            mo.send_message([0x90 | ch, 60, 100])
            time.sleep(0.05)
            mo.send_message([0x80 | ch, 60, 0])
            mo.send_message([0xB0 | ch, 1, 64])
            time.sleep(0.05)
            mo.send_message([0xB0 | ch, 1, 0])
            time.sleep(0.05)
        time.sleep(0.5)
        mo.close_port()
        print('\nsent notes and mod wheel on channels %s out of %r:' % (a.channels, out))
        with lock:
            for n in ins:
                got = [m.hex() for m in heard[n] if m[0] != 0xF8]
                print('   %r heard %d: %s' % (n, len(got), ' '.join(got[:16])))
                heard[n].clear()
    for mi in opened:
        mi.close_port()


if __name__ == '__main__':
    sys.exit(main())
