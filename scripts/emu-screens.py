#!/usr/bin/env python3
"""Screens of the EXPRESSION page for the README, from the firmware in digiemu.

    set -a; . toolchain/emu-NAME.env; set +a
    digiemu/.venv/bin/python scripts/emu-screens.py      -> docs/img/*.png, module/midi-expression/media/page-*.png
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dtsession
from emu.session import Session, run_script
from emu.panel import write_png

syx = os.environ['DT2_SYX']; snap = os.path.abspath(os.environ['DT2_SNAPSHOT'])
out = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs', 'img'))
os.makedirs(out, exist_ok=True)
os.chdir(os.path.dirname(syx))
io = dtsession.Midi()
s = Session(snap, syx, audio=False, on_machine=io.on_machine)
uc = s.m.uc
PARAMS = 0x421c9188


def do(text):
    for st in dtsession.parse(text):
        run_script(s, [st])


MEDIA = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'module', 'midi-expression', 'media'))


def shot(name, media=None):
    write_png(s.screen_at(s.now_ms()), '%s/%s.png' % (out, name), scale=2)
    if media:                                     # the module's own screenshots, as Modwerk shows them
        write_png(s.screen_at(s.now_ms()), '%s/%s.png' % (MEDIA, media), scale=3)


def turn(knob, n):
    s.turn(s.encoder(knob), n)
    s.run_ms(250)


s.run_ms(300)
mem = bytes(uc.mem_read(0x47be0000, 0x20000))
audio = 0x47be0000 + mem.find(bytes([2, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0, 0]) * 8)
midi = 0x47be0000 + mem.find(bytes([1, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0, 0]) * 8)


def cfg(base, t, a, mw, mwa, vel, vela, at, ata, sens):
    uc.mem_write(base + 12 * t, struct.pack('>bbbbbxhhh', a, mw, at, vel, sens, mwa * 256, ata * 256, vela * 256))


# Audio track 1: bend 2 semitones, V.SNS 80 %, MW -> filter frequency +32,
# VEL -> AMP volume +24, AT -> filter resonance +16.
cfg(audio, 0, 2, 26, 32, 45, 24, 27, 16, 80)
do('tap TRIG\nwait 400\n')
shot('trig-page')
do('tap TRIG\nwait 600\n')
turn('B', 1); turn('B', -1)                      # a frame with the values as set
s.run_ms(2500)
shot('expression-audio', 'page-audio')
turn('E', 1)
shot('expression-audio-list')
s.run_ms(2500)
# MIDI track A: THRU on, MW -> CC VAL2 (SEL2 = CC 74) +32, VEL -> BC +64, AT -> LFO depth +40.
uc.mem_write(PARAMS + 2 * (33 + 1), bytes([74, 0]))
cfg(midi, 0, 1, 6, 32, 4, 64, 17, 40, 100)
do('press TRK 100\ntap 9\nrelease TRK 100\nwait 300\npress FUNC 100\ntap A\nrelease FUNC 100\nwait 300\ntap TRIG\nwait 400\ntap TRIG\nwait 600\n')
turn('B', -1); turn('B', 1)
s.run_ms(2500)
shot('expression-midi', 'page-midi')
turn('G', -1)
shot('expression-midi-list')
s.run_ms(2500)
turn('C', -1)
shot('expression-midi-list-cc')
# The preset menu: a 1-second push of knob A on MIDI track A's page.
do('press #40 4000\n')                                          # (the emulator's UI ticks run slower than a unit's)
s.run_ms(500)
shot('menu')
do('tap DOWN\nwait 300\ntap YES\nwait 5000\n')                 # SAVE: the slots
shot('menu-save')
do('tap YES\nwait 400\n')                                         # slot 01: the name
for k in ('DOWN', 'DOWN', 'DOWN', 'RIGHT', 'DOWN', 'RIGHT', 'RIGHT', 'UP'):
    do('tap %s\nwait 400\n' % k)
shot('menu-name')
do('press FUNC 100\nwait 300\ntap RIGHT\nwait 300\ntap DOWN\nwait 300\n')   # FUNC held: the letters
shot('menu-letters')
do('release FUNC 100\nwait 300\n')
do('tap YES\nwait 600\n')                                         # saved
shot('menu-saved')
do('wait 3000\ntap UP\nwait 300\ntap YES\nwait 6000\n')       # LOAD: the list, with it
shot('menu-load')
print('screens in', out)
