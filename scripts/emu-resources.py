#!/usr/bin/env python3
"""MIDI Expression's resources on the firmware, in digiemu.

1. Memory that leaks: the firmware's heap (operator new 0x400d412c, delete
   0x400d4332; its state words 0x4020bec4..) is watched while the mod writes
   and reads its files a few hundred times, against the same time idle.
2. CPU: instructions executed in the module area (the mod and the core that
   calls it) per emulated second, idle and under a heavy load (every route of
   every track live, a flood of pitch bend, aftertouch, CC 1, CC 2 and notes),
   as a share of the emulated CPU's instructions a second.

    set -a; . toolchain/emu-NAME.env; set +a
    digiemu/.venv/bin/python scripts/emu-resources.py memory
    digiemu/.venv/bin/python scripts/emu-resources.py cpu
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dtsession
from emu.session import Session, run_script
from unicorn import UC_HOOK_CODE

syx = os.environ['DT2_SYX']; snap = os.path.abspath(os.environ['DT2_SNAPSHOT'])
os.chdir(os.path.dirname(syx))
io = dtsession.Midi()
s = Session(snap, syx, audio=True, on_machine=io.on_machine)
uc = s.m.uc
MOD_LO, MOD_HI = 0x47b00000, 0x47c00000
NEW, DELETE, HEAP = 0x400d412c, 0x400d4332, 0x4020bec4
calls = {'new': 0, 'delete': 0, 'mod': 0}
counting = [False]


def on_new(u, a, sz, _):
    calls['new'] += 1


def on_delete(u, a, sz, _):
    calls['delete'] += 1


def on_mod(u, a, sz, _):
    if counting[0]:
        calls['mod'] += 1


uc.hook_add(UC_HOOK_CODE, on_new, begin=NEW, end=NEW)
uc.hook_add(UC_HOOK_CODE, on_delete, begin=DELETE, end=DELETE)


PHASES = sys.argv[1:] or ['memory', 'cpu']
if 'cpu' in PHASES:     # before anything runs: a hook added later misses code already translated
    uc.hook_add(UC_HOOK_CODE, on_mod, begin=MOD_LO, end=MOD_HI - 1)


def do(text):
    for st in dtsession.parse(text):
        run_script(s, [st])


s.run_ms(300)
mem = bytes(uc.mem_read(0x47be0000, 0x20000))
probe = 0x47be0000 + mem.find(struct.pack('>I', 0x4d455850))
audio = 0x47be0000 + mem.find(bytes([2, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0, 0]) * 8)
midi = 0x47be0000 + mem.find(bytes([1, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0, 0]) * 8)
assert probe > 0x47be0000 and audio > 0x47be0000 and midi > 0x47be0000


def pr():
    return struct.unpack('>7I', bytes(uc.mem_read(probe, 28)))


def heap():
    return struct.unpack('>6I', bytes(uc.mem_read(HEAP, 24)))


def cfg(base, t, a, mw, mwa, vel, vela, at, ata, sens):
    uc.mem_write(base + 12 * t, struct.pack('>bbbbbxhhh', a, mw, at, vel, sens, mwa * 256, ata * 256, vela * 256))


lines = []


def say(t):
    print(t, flush=True)
    lines.append(t)


if 'memory' in PHASES:
    # ---- 1. memory ------------------------------------------------------------------------
    s.run_ms(3000)                                  # let the boot's own work settle
    h0, c0 = heap(), dict(calls)
    s.run_ms(10000)
    idle_new, idle_del = calls['new'] - c0['new'], calls['delete'] - c0['delete']
    h1 = heap()
    say('idle 10 s: %d new, %d delete; heap words %s -> %s' % (idle_new, idle_del, [hex(x) for x in h0], [hex(x) for x in h1]))
    c1, w0, r0 = dict(calls), pr()[2], pr()[4]
    for i in range(40):                             # 40 changes: current.txt written each time
        cfg(audio, i % 8, 2 + i % 5, 26, i % 64, 0, 0, 0, 0, 100)
        s.run_ms(250)
        while pr()[2] == w0 + i and s.now_ms() < 10 ** 9:
            s.run_ms(250)
    for i in range(40):                             # 40 power-on reads (config and current.txt)
        uc.mem_write(probe + 4, struct.pack('>I', 0))
        s.run_ms(150)
    h2 = heap()
    work_new, work_del = calls['new'] - c1['new'], calls['delete'] - c1['delete']
    say('%d file writes, %d reads: %d new, %d delete (balance %d); heap words %s' % (
        pr()[2] - w0, pr()[4] - r0, work_new, work_del, work_new - work_del, [hex(x) for x in h2]))
    leak = (work_new - work_del) - (idle_new - idle_del)
    say('%s no memory kept by the mod\'s file work (new - delete beyond idle: %d)' % ('PASS' if leak <= 0 else 'FAIL', leak))
    say('%s no failed write, no bad file (%d, %d)' % ('PASS' if not pr()[3] and not pr()[5] else 'FAIL', pr()[3], pr()[5]))

if 'cpu' in PHASES:
    # ---- 2. CPU ------------------------------------------------------------------------------
    ips = getattr(s, 'ips', 0)




    def share(ms):
        calls['mod'] = 0
        counting[0] = True
        t0 = s.now_ms()
        s.run_ms(ms)
        counting[0] = False
        sec = (s.now_ms() - t0) / 1000.0
        return calls['mod'] / sec


    idle = share(2000)
    say('idle: %.0f instructions/s in the module area (%.2f %% of %d/s)' % (idle, 100.0 * idle / ips, ips))
    for t in range(8):                              # every route of every track live
        cfg(audio, t, 12, 26, 32, 45, 24, 27, 16, 50)
        cfg(midi, t, 1, 6, 32, 4, 64, 17, 40, 50)
    do('press FUNC 100\ntap A\nrelease FUNC 100\nwait 300\n')   # (track 1 selected: the auto channel plays it)


    def flood(ms, ch):
        calls['mod'] = 0
        counting[0] = True
        t0 = s.now_ms()
        i = 0
        while s.now_ms() - t0 < ms:
            v = (i * 37) % 128
            for msg in (bytes([0xE0 | ch, v, (i * 5) % 128]), bytes([0xD0 | ch, v]), bytes([0xB0 | ch, 1, v]),
                        bytes([0xB0 | ch, 2, 127 - v]), bytes([0x90 | ch, 48 + i % 24, 1 + v]), bytes([0x80 | ch, 48 + (i + 12) % 24, 0])):
                io.send(s, msg)
            s.run_ms(6)                             # six messages in 6 ms: about the DIN wire rate
            i += 1
        counting[0] = False
        sec = (s.now_ms() - t0) / 1000.0
        return calls['mod'] / sec


    a = flood(2000, 13)
    say('audio track, every route live, a flood at the wire rate: %.0f instructions/s (%.2f %%)' % (a, 100.0 * a / ips))
    do('press TRK 100\ntap 9\nrelease TRK 100\nwait 300\npress FUNC 100\ntap A\nrelease FUNC 100\nwait 300\n')
    m = flood(2000, 13)
    say('MIDI track A, every route live, the same flood: %.0f instructions/s (%.2f %%)' % (m, 100.0 * m / ips))
    say('static RAM: see the image\'s ramSpare (module area left free)')
