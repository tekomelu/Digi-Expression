"""0.13 in digiemu: the power-on file (current.txt) is not written while the
sequencer plays, even an empty pattern, and is written once it stops. A setting
is changed on the EXPRESSION page through the panel (SETTINGS > EXPRESSION,
knob A); the store's probe counts the files written.

    set -a; . toolchain/emu-NAME.env; set +a
    digiemu/.venv/bin/python scripts/emu-save-playing.py
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dtsession
from emu.session import Session, run_script

syx = os.environ['DT2_SYX']; snap = os.path.abspath(os.environ['DT2_SNAPSHOT'])
os.chdir(os.path.dirname(syx))
s = Session(snap, syx, audio=False, on_machine=dtsession.Midi().on_machine)
uc = s.m.uc
fails = []


def check(ok, what):
    print('%s %s' % ('PASS' if ok else 'FAIL', what))
    if not ok:
        fails.append(what)


def do(text):
    for st in dtsession.parse(text):
        run_script(s, [st])


s.run_ms(300)
mem = bytes(uc.mem_read(0x47be0000, 0x20000))
probe = 0x47be0000 + mem.find(struct.pack('>I', 0x4d455850))


blank = bytes([0, 0, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0])
audio = 0x47be0000 + mem.find(blank * 16) + 12 * 8   # the MIDI table comes first, then audio


def vsns():
    return bytes(uc.mem_read(audio + 5, 1))[0]


def written():
    return struct.unpack('>7I', bytes(uc.mem_read(probe, 28)))[2]


def playing():
    return bytes(uc.mem_read(0x4199ebb3, 1))[0]


do('wait 3000\n')                                     # boot's own files settle
do('tap PLAY\nwait 500\n')
check(playing() == 1, 'the sequencer runs (flag %d)' % playing())
w0 = written()
do('tap GLOBAL\nwait 600\n' + 'tap DOWN\nwait 150\n' * 10 + 'tap YES\nwait 600\n' + 'press FUNC 100\nturn B -1 300\nturn B -1 300\nrelease FUNC 300\nwait 1500\n')   # FUNC + turn: 100 % to 75 %
check(vsns() < 100, 'V.SNS turned down on the page (%d %%)' % vsns())
do('tap NO\nwait 300\ntap NO\nwait 300\n')
do('wait 8000\n')                                     # four times the 2 s quiet time, playing an empty pattern
check(written() == w0, 'playing: current.txt not written (%d writes)' % (written() - w0))
do('tap STOP\nwait 300\n')
check(playing() == 0, 'stopped (flag %d)' % playing())
do('wait 4000\n')
check(written() == w0 + 1, 'stopped: current.txt written once (%d writes)' % (written() - w0))
print('%d failures' % len(fails))
sys.exit(1 if fails else 0)
