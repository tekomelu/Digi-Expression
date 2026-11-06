#!/usr/bin/env python3
"""Does the MIDI interface itself lose messages when a clock byte lands inside them?

No Digitakt: one MIDI cable from the interface's OUT straight to its own IN.
Sends the same pattern as hwqual.py's clockloss phase (a pitch bend and a note
back to back), first with no clock, then with a clock byte sent 0.1 to 0.7 ms
after the pair, so on the cable it lands among the first message's bytes. Every
message should come back exactly as sent.

    ~/.venvs/dt/bin/python scripts/loopback.py --port "Portti 1"
"""
import argparse
import random
import sys
import threading
import time

import rtmidi

PERIOD = 60 / 160 / 24


def at(t):
    while True:
        left = t - time.perf_counter()
        if left <= 0:
            return
        if left > 0.002:
            time.sleep(left - 0.0015)


def find(ports, name):
    for i, p in enumerate(ports):
        if name.lower() in p.lower():
            return i
    sys.exit('no port matching %r in %s' % (name, ports))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', required=True, help='part of the interface port name, as hwqual.py --list shows it')
    ap.add_argument('-n', type=int, default=300)
    a = ap.parse_args()
    mi, mo = rtmidi.MidiIn(), rtmidi.MidiOut()
    mi.ignore_types(sysex=True, timing=False, active_sense=True)
    mi.open_port(find(mi.get_ports(), a.port))
    mo.open_port(find(mo.get_ports(), a.port))
    rx, lock = [], threading.Lock()
    mi.set_callback(lambda ev, data=None: (lock.acquire(), rx.append(bytes(ev[0])), lock.release()))
    print('in: %s\nout: %s\ncable from the interface OUT to its own IN, no Digitakt\n' % (
        mi.get_port_name(find(mi.get_ports(), a.port)), mo.get_port_name(find(mo.get_ports(), a.port))))
    total_bad = 0
    for label, clock, first in (('no clock', False, 'PB'), ('clock byte inside the pitch bend', True, 'PB'),
                                ('clock byte inside the note', True, 'note')):
        lost = {'PB': 0, 'note': 0}
        clocks_back = 0
        nxt = time.perf_counter() + PERIOD
        for k in range(a.n):
            v = 8192 + (k % 2 * 2 - 1) * (300 + (k * 53) % 7000)
            pb = bytes((0xE8, v & 127, (v >> 7) & 127))
            note = bytes((0x9F, 48 + k % 48, 90))
            with lock:
                rx.clear()
            at(nxt - (0.0001 + random.random() * 0.0006))
            for x in ((pb, note) if first == 'PB' else (note, pb)):
                mo.send_message(list(x))
            at(nxt)
            if clock:
                mo.send_message([0xF8])
            nxt += PERIOD
            at(nxt - PERIOD / 2)
            mo.send_message([0x8F, note[1], 0])
            at(nxt)
            nxt += PERIOD
            at(nxt - PERIOD / 2)
            with lock:
                got = list(rx)
            lost['PB'] += pb not in got
            lost['note'] += note not in got
            clocks_back += sum(1 for m in got if m == b'\xf8')
        bad = lost['PB'] + lost['note']
        total_bad += bad
        print('%-34s pitch bends lost %d of %d, notes lost %d of %d%s' % (
            label + ':', lost['PB'], a.n, lost['note'], a.n,
            ', clock bytes back %d of %d' % (clocks_back, a.n) if clock else ''))
    mi.close_port()
    mo.close_port()
    print('\n' + ('the interface passed everything' if not total_bad else 'the interface itself lost messages'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
