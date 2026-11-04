#!/usr/bin/env python3
"""Drive the emulated Digitakt from a step script and save screen captures.

Uses digiemu's Session (emu/session.py): the same machine the app builds,
stepped in emulated time, with key codes measured for the mk1 (the device
file's [panel.labels], not the firmware's panel-test name table, whose order
is not the runtime key map on the mk1).

Script lines use emu/fwcompare.py's verbs:

    tap GLOBAL            # tap a key (labels: PLAY, STOP, FUNC, TRK, 1..16, ...)
    press FUNC            # hold a key
    release FUNC
    turn A 3              # turn encoder A three detents
    wait 500              # run 500 emulated ms
    snap settings         # save the current screen as <out>/NN-settings.png

    source env.sh; set -a; . toolchain/emu-dt154.env; set +a
    digiemu/.venv/bin/python scripts/dtsession.py steps.txt captures/run1
    digiemu/.venv/bin/python scripts/dtsession.py -e "tap GLOBAL; snap settings" captures/run1

Run it from anywhere; it changes into the firmware folder ($DT2_SYX's) itself.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DIGIEMU = os.path.abspath(os.environ.get('DIGIEMU_DIR', os.path.join(HERE, '..', 'digiemu')))
sys.path.insert(0, DIGIEMU)

from emu import fwcompare, panel  # noqa: E402
from emu.session import Session, run_script  # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('script', nargs='?', help='step script file')
    ap.add_argument('out', help='directory for PNG captures')
    ap.add_argument('-e', '--steps', help='inline steps, separated by ";"')
    ap.add_argument('--snapshot', help='default: $DT2_SNAPSHOTS/Digitakt_OS1.54/gui.snap')
    ap.add_argument('--settle', type=int, default=300, help='ms to run before the first step')
    ap.add_argument('--audio', action='store_true', help='also run the audio path')
    args = ap.parse_args(argv)

    if args.steps:
        text = args.steps.replace(';', '\n')
    elif args.script:
        text = open(args.script).read()
    else:
        ap.error('give a script file or -e')
    steps = fwcompare.parse_script(text)

    syx = os.environ['DT2_SYX']
    snapshot = args.snapshot or os.path.join(os.environ['DT2_SNAPSHOTS'], 'Digitakt_OS1.54', 'gui.snap')
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    os.chdir(os.path.dirname(syx))

    s = Session(snapshot, syx, audio=args.audio)
    try:
        s.run_ms(args.settle)
        marks = [('start', s.ms)] + list(run_script(s, steps))
        for i, (label, ms) in enumerate(marks):
            buf = s.screen_at(ms)
            if buf is None:
                print('%-12s %8.1f ms  no frame yet' % (label, ms))
                continue
            path = os.path.join(out, '%02d-%s.png' % (i, label))
            panel.write_png(buf, path, scale=6)
            print('%-12s %8.1f ms  %4d lit  -> %s' % (label, ms, len(panel.lit(buf)), path))
        if s.halted:
            print('HALTED:', s.halted)
            return 1
    finally:
        s.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
