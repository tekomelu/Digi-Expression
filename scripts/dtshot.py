#!/usr/bin/env python3
"""Run digiemu's headless panel driver (tools/dtdrive.py) on a snapshot that
digiemu's app saved.

The app builds the emulator with unblock, softfloat, bitmap and dsp on, and a
snapshot only reopens with the flags it was saved with. dtdrive.py passes
none, so this wrapper adds them and otherwise runs dtdrive.py unchanged:

    source env.sh
    set -a; . toolchain/emu-dt154.env; set +a
    digiemu/.venv/bin/python scripts/dtshot.py --syx "$DT2_SYX" \
        --snapshot "$DT2_SNAPSHOTS/Digitakt_OS1.54/gui.snap" \
        --press SETTINGS --png captures/settings

Run it from the firmware folder (the directory of $DT2_SYX), as digiemu's
app does.
"""
import os
import runpy
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DIGIEMU = os.environ.get('DIGIEMU_DIR', os.path.join(HERE, '..', 'digiemu'))
sys.path.insert(0, os.path.abspath(DIGIEMU))

from emu import uiresume  # noqa: E402

GUI_FLAGS = dict(unblock=True, softfloat=True, bitmap=True, dsp=True)
_open = uiresume.open_snapshot


def open_snapshot(*args, **kwargs):
    for key, value in GUI_FLAGS.items():
        kwargs.setdefault(key, value)
    return _open(*args, **kwargs)


uiresume.open_snapshot = open_snapshot
sys.argv[0] = os.path.join(DIGIEMU, 'tools', 'dtdrive.py')
runpy.run_path(sys.argv[0], run_name='__main__')
