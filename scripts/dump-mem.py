#!/usr/bin/env python3
"""Write emulator memory to a file after boot (research aid), e.g. to
disassemble the running OS:

    digiemu/.venv/bin/python scripts/dump-mem.py 0x400d5000 0x2000 /tmp/x.bin
    m68k-elf-objdump -b binary -m m68k:cfv4e --adjust-vma=0x400d5000 -D /tmp/x.bin
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dtsession  # noqa: E402,F401  (sets up digiemu's path)
from emu.session import Session  # noqa: E402


def main():
    addr, size, out = int(sys.argv[1], 0), int(sys.argv[2], 0), os.path.abspath(sys.argv[3])
    syx = os.environ['DT2_SYX']
    snap = os.path.abspath(os.environ.get('DT2_SNAPSHOT') or os.path.join(
        os.environ['DT2_SNAPSHOTS'], 'Digitakt_OS1.54', 'gui.snap'))
    os.chdir(os.path.dirname(syx))
    s = Session(snap, syx, audio=False)
    try:
        s.run_ms(10)
        open(out, 'wb').write(bytes(s.m.uc.mem_read(addr, size)))
    finally:
        s.close()


if __name__ == '__main__':
    main()
