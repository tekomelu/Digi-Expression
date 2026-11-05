#!/usr/bin/env python3
"""Is the loop wired? Sends a note, a bend and the mod wheel on the auto
channel to the Digitakt's MIDI IN and prints what its MIDI OUT sends back.

    ~/.venvs/dt/bin/python scripts/hwping.py "USB Midi 4i4o Portti 1"
    ~/.venvs/dt/bin/python scripts/hwping.py --list
"""
import sys
import time

import rtmidi

AUTO = 10


def ports(cls):
    return cls().get_ports()


def main():
    if len(sys.argv) < 2 or sys.argv[1] == '--list':
        print('outputs (to the Digitakt):', ports(rtmidi.MidiOut))
        print('inputs (from the Digitakt):', ports(rtmidi.MidiIn))
        return
    name = sys.argv[1]
    out_name = sys.argv[2] if len(sys.argv) > 2 else name
    mi, mo = rtmidi.MidiIn(), rtmidi.MidiOut()
    mi.ignore_types(timing=True, sysex=True, active_sense=True)
    mi.open_port([p for p in mi.get_ports() if name in p][0])
    mo.open_port([p for p in mo.get_ports() if out_name in p][0])
    got = []
    mi.set_callback(lambda m, _: got.append(m[0]))
    c = AUTO - 1
    for msg in ([0x90 | c, 60, 100], [0xE0 | c, 0, 96], [0xB0 | c, 1, 50], [0x80 | c, 60, 0], [0xE0 | c, 0, 64], [0xB0 | c, 1, 0]):
        mo.send_message(msg)
        time.sleep(0.15)
    time.sleep(0.3)
    print('sent on channel %d: note 60, bend up, wheel 50, then back' % AUTO)
    print('came back: %s' % (' '.join(bytes(m).hex() for m in got) or 'NOTHING'))
    if not got:
        print('-> nothing reaches the Digitakt from this port, or nothing comes back: check that this '
              'port\'s MIDI OUT socket on the interface goes to the Digitakt\'s MIDI IN, and its MIDI OUT to this port\'s MIDI IN.')


if __name__ == '__main__':
    main()
