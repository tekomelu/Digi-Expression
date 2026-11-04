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

import struct  # noqa: E402

from emu import dtmap, uiresume  # noqa: E402

GUI_FLAGS = dict(unblock=True, softfloat=True, bitmap=True, dsp=True)
_open = uiresume.open_snapshot
_names = dtmap.names
LOAD = 0x40000400


def find_name_table(img, load=LOAD):
    """-> address of the firmware's 48-entry control-name table, found by its
    shape (48 pointers to short printable names, PLAY and FUNC among them).
    digiemu's dtmap.names() has the OS 1.53 address fixed; on 1.54 the table
    sits 0x300 bytes later (0x4018e554)."""
    def name(ptr):
        o = ptr - load
        if not 0 <= o < len(img):
            return None
        end = img.find(b'\x00', o)
        if end <= o or end - o > 24:
            return None
        text = img[o:end]
        return text.upper() if all(32 <= c < 127 for c in text) else None

    for off in range(0, len(img) - 48 * 4, 4):
        first = struct.unpack_from('>I', img, off)[0]
        if not load <= first < load + len(img):
            continue
        found = []
        for i in range(48):
            n = name(struct.unpack_from('>I', img, off + 4 * i)[0])
            if n is None:
                break
            found.append(n)
        if len(found) == 48 and b'PLAY' in found and b'FUNC' in found:
            return load + off
    return None


def names(img, base=None, load=LOAD):
    if base is None:
        base = find_name_table(img, load)
        if base is None:
            return _names(img, load=load)
    return _names(img, base=base, load=load)


dtmap.names = names


def open_snapshot(*args, **kwargs):
    for key, value in GUI_FLAGS.items():
        kwargs.setdefault(key, value)
    return _open(*args, **kwargs)


uiresume.open_snapshot = open_snapshot
sys.argv[0] = os.path.join(DIGIEMU, 'tools', 'dtdrive.py')
runpy.run_path(sys.argv[0], run_name='__main__')
