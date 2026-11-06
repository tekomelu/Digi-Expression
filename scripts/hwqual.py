#!/usr/bin/env python3
"""Hardware qualification for MIDI Expression (Digitakt mk1, OS 1.54).

The test project EXPR TEST (docs/TEST-SETUP.md, or --setup) sets each of the
eight MIDI tracks up to test a different part of the mod. The program plays
the keyboard on every track's channel, records everything the Digitakt sends
back and checks each value against a model of the firmware's sums (expr.c).
Connected over DIN and USB at once (--in2/--out2), it sends on one, listens on
both, then swaps: DIN in to DIN and USB out, USB in to both, compared message
for message. Where only eyes can tell (the page, presets, saving), it asks
yes / no / skip; every answer can take a note.

    python3 -m venv ~/.venvs/dt && ~/.venvs/dt/bin/pip install python-rtmidi
    hwqual.py --setup                    # the test project (docs/TEST-SETUP.md)
    hwqual.py --list                     # the MIDI ports
    hwqual.py --in "Interface" --out "Interface" --in2 "Digitakt" --out2 "Digitakt" --label full

Phases (--only takes a comma list or a group):
  core     tracks (every MIDI track, every source), notes, multi, latency
  play     seq, flood (the program starts and stops the sequencer itself)
  persist  loading, a power cycle, an unsaved change, CREATE NEW
  ui       the page and the presets, by eye
  all      core, play, persist, ui (the default)
  ears     audio track 1 by ear (not in all)
  auto     the auto channel (not in all; its own set-up, see docs/TEST-SETUP.md)

Writes results/hardware/qual-<label>-<date-time>/: report.json, summary.md (for
the Modwerk hardware report), log.txt, midi.jsonl (every message, every port,
timestamped). Ctrl+C stops and still saves. Nothing here sends SysEx or writes
to the Digitakt's memory.
"""
import argparse
import json
import os
import random
import statistics
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from hwtest import Io, ask, hexs, wait, ROOT  # noqa: E402

VERSION = '0.12.4-experimental'
SETTLE = 0.06                   # the Digitakt answers in about 1..40 ms
SETUP_DOC = os.path.join(ROOT, 'docs', 'TEST-SETUP.md')
D = 256                         # depths in 1/256: 64.00 = 64 * D


def tdiv(a, b):
    """C integer division (toward zero), as the firmware computes."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def round256(v):
    return (v + 128) >> 8 if v >= 0 else -((-v + 128) >> 8)


def route(src, amount):
    """A route's part in 1/256 of a MIDI step: expr.c live_parts."""
    return tdiv(src * amount, 64)


def r7(src, depth):
    """A route's part on a 7-bit value (CC, aftertouch), in MIDI steps."""
    return round256(route(src, depth * D))


def r14(src, depth):
    """A route's part on pitch bend, in 14-bit steps."""
    return tdiv(route(src, depth * D) * 16383, 32512)


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def sens(v, s):
    """V.SNS on a velocity: audio.c me_velocity_scaled."""
    if s >= 100 or v <= 0:
        return v
    return clamp(v + ((127 - v) * (100 - s) + 50) // 100, 1, 127)


# The eight MIDI tracks of EXPR TEST (docs/TEST-SETUP.md). s is the live input
# state: PB as an offset from the centre, AT, MW, BC, VEL (the last note's).
# Each output is what the track adds to its own value; a key not listed must
# never come out on that track's channel.
TRACKS = {
    'A': {'vsens': 100, 'out': {
        'PB': lambda s: s['PB'], 'AT': lambda s: s['AT'], 'CC1': lambda s: s['MW'], 'CC2': lambda s: s['BC']}},
    'B': {'vsens': 50, 'vel_cc': 'CC2', 'out': {
        'CC74': lambda s: r7(s['MW'], 64), 'CC1': lambda s: s['MW'],
        'PB': lambda s: s['PB'] + r14(s['AT'], 32), 'AT': lambda s: s['AT'],
        'CC2': lambda s: s['BC'] + r7(s['VEL'], 32)}},
    'C': {'vsens': 100, 'out': {'CC1': lambda s: r7(s['MW'], -32)}},
    'D': {'vsens': 0, 'out': {
        'PB': lambda s: s['PB'] + r14(s['MW'], 64), 'CC1': lambda s: s['MW'],
        'CC71': lambda s: r7(s['AT'], -64), 'AT': lambda s: s['AT'], 'CC2': lambda s: s['BC']}},
    'E': {'vsens': 100, 'out': {
        'AT': lambda s: s['AT'] + r7(s['MW'], 64), 'CC1': lambda s: s['MW'],
        'PB': lambda s: s['PB'], 'CC2': lambda s: s['BC']}},
    'F': {'vsens': 25, 'out': {'CC1': lambda s: r7(s['AT'], -64)}},
    'G': {'vsens': 100, 'out': {
        'CC2': lambda s: s['BC'] + r7(s['AT'], 32), 'AT': lambda s: s['AT'],
        'PB': lambda s: s['PB'], 'CC1': lambda s: s['MW']}},
    'H': {'vsens': 100, 'out': {}},
}
NAMES = 'ABCDEFGH'
INPUTS = ('PB', 'AT', 'MW', 'BC')


CLOCK_GUARD = 0.0004     # s after a clock byte before anything else goes out (see locked_send)


def locked_send(raw, lock):
    """The clock thread and the program share a port. Some MIDI interfaces (the 4i4o used on
    7 Nov 2026, scripts/loopback.py) drop the message that follows a clock byte within about
    0.3 ms, on their DIN output: so nothing else goes out until CLOCK_GUARD after one."""
    last = [0.0]

    def send(data):
        with lock:
            if data != b'\xf8':
                while time.perf_counter() - last[0] < CLOCK_GUARD:
                    pass
            r = raw(data)
            if data == b'\xf8':
                last[0] = time.perf_counter()
            return r
    return send


class Qual:
    def __init__(self, io, mirror, cfg, out_dir, label):
        self.io, self.mirror, self.cfg, self.out_dir, self.label = io, mirror, cfg, out_dir, label
        self.ports = [p for p in (io, mirror) if p]
        self.results, self.log, self.extra = [], [], {}
        self.t_start = time.time()
        self.heard = {}                 # (out channel, key) -> last value heard on the listening port
        self.own = {}                   # (track, key) -> the track's own value, as learned
        self.state = {t: self.zero() for t in NAMES}
        self.seen = 0
        self.fails = []
        self._clock = None
        for p in self.ports:            # the clock thread and the program share the ports
            p.send = locked_send(p.send, threading.Lock())

    @staticmethod
    def zero():
        return {'PB': 0, 'AT': 0, 'MW': 0, 'BC': 0, 'VEL': 0}

    # ---- bookkeeping ---------------------------------------------------------
    def note(self, text):
        print(text)
        self.log.append(text)

    def check(self, phase, name, ok, detail=''):
        status = 'PASS' if ok is True else 'FAIL' if ok is False else str(ok)   # or SKIP, INFO
        self.note('  %-5s %s%s' % (status, name, ('  (' + detail + ')') if detail else ''))
        self.results.append({'phase': phase, 'check': name, 'status': status, 'detail': detail,
                             'label': self.label, 'via': self.io.out_name, 't': round(time.time() - self.t_start, 1)})

    def answer(self, phase, question):
        while True:
            a = ask(question + '  [y / n / s(kip)] (a note may follow)').strip()
            if a[:1].lower() in ('y', 'n', 's'):
                st = {'y': True, 'n': False, 's': 'SKIP'}[a[:1].lower()]
                self.check(phase, question, st, a[1:].strip())
                return st

    # ---- MIDI ----------------------------------------------------------------
    def tin(self, t):
        return self.cfg['in'][NAMES.index(t)]

    def tout(self, t):
        return self.cfg['out'][NAMES.index(t)]

    def send(self, *msgs, gap=0.002):
        for m in msgs:
            self.io.send(bytes(m))
            if gap:
                wait(gap)

    def fold(self):
        with self.io.lock:
            new = self.io.rx[self.seen:]
            self.seen += len(new)
        got = [(t, m) for t, m in new if m[0] < 0xF0]
        for _, m in got:
            k = self.key(m)
            if k:
                self.heard[k] = self.value(m)
        return got

    def settle(self, t=SETTLE):
        wait(t)
        return self.fold()

    @staticmethod
    def key(m):
        s, ch = m[0] & 0xF0, m[0] & 15
        if s == 0xE0 and len(m) > 2:
            return (ch, 'PB')
        if s == 0xD0 and len(m) > 1:
            return (ch, 'AT')
        if s == 0xB0 and len(m) > 2:
            return (ch, 'CC%d' % m[1])
        return None

    @staticmethod
    def value(m):
        s = m[0] & 0xF0
        if s == 0xE0:
            return m[1] | (m[2] << 7)
        return m[1] if s == 0xD0 else m[2]

    def msg(self, t, kind, v):
        ch = self.tin(t)
        if kind == 'PB':
            v14 = clamp(v + 8192, 0, 16383)
            return (0xE0 | ch, v14 & 127, v14 >> 7)
        if kind == 'AT':
            return (0xD0 | ch, v)
        return (0xB0 | ch, 1 if kind == 'MW' else 2, v)

    def play(self, t, kind, v, gap=0.002):
        """One live input to track t: sent, and kept in the model's state."""
        self.state[t][kind] = v
        self.send(self.msg(t, kind, v), gap=gap)

    def mark(self):
        return [p.mark() for p in self.ports]

    def mirrored(self, phase, marks, name, info=False):
        """Both listening ports heard the same messages (OUTPUT TO MIDI+USB). Under a flood the
        DIN output (31,250 baud) can fall behind USB: there it is a measurement, not a check."""
        if not self.mirror:
            return
        wait(0.15)
        a = [m for _, m in self.io.since(marks[0]) if m[0] < 0xF0]
        b = [m for _, m in self.mirror.since(marks[1]) if m[0] < 0xF0]
        diff = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), None)
        ok = len(a) == len(b) and diff is None
        detail = '%d messages each' % len(a) if ok else '%d on %s, %d on %s%s' % (
            len(a), self.io.in_name, len(b), self.mirror.in_name,
            ', first difference at %d: %s / %s' % (diff, hexs(a[diff]), hexs(b[diff])) if diff is not None else '')
        self.check(phase, '%s: %s and %s heard the same' % (name, self.io.in_name, self.mirror.in_name),
                   ok if not info else ('PASS' if ok else 'INFO'), detail)

    # ---- the model -----------------------------------------------------------
    def want(self, t, k):
        own = self.own.get((t, k))
        if own is None:
            return None
        lo, hi = (0, 16383) if k == 'PB' else (0, 127)
        return clamp(own + TRACKS[t]['out'][k](self.state[t]), lo, hi)

    def compare(self, t, label, got=()):
        """Learn an own value from an unclamped answer; compare every output of track t."""
        o = self.tout(t)
        for k, fn in TRACKS[t]['out'].items():
            lo, hi = (0, 16383) if k == 'PB' else (0, 127)
            if (t, k) not in self.own:
                ans = [self.value(m) for _, m in got if self.key(m) == (o, k)]
                if ans and lo < ans[-1] < hi:
                    self.own[(t, k)] = ans[-1] - fn(self.state[t])
                continue
            w, h = self.want(t, k), self.heard.get((o, k))
            if h is not None and w != h:
                self.fails.append('%s %s: want %s, heard %s' % (label, k, w, h))

    def stray(self, t, msgs):
        """Messages on track t's channel the model says it never sends."""
        o, keys = self.tout(t), set(TRACKS[t]['out'])
        return [hexs(m) for _, m in msgs if (m[0] & 15) == o and self.key(m) and self.key(m)[1] not in keys]

    def reset_inputs(self):
        for t in NAMES:
            for k in INPUTS:
                self.play(t, k, 0, gap=0.001)
        self.settle(0.25)

    # ---- transport -------------------------------------------------------------
    def transport(self, run, bpm=160):
        if run:
            stop = threading.Event()

            def tick():
                nxt = time.perf_counter()
                while not stop.is_set():
                    self.io.send(b'\xf8')
                    nxt += 60 / bpm / 24
                    time.sleep(max(0, nxt - time.perf_counter()))
            th = threading.Thread(target=tick, daemon=True)
            self._clock = (stop, th)
            th.start()
            wait(0.3)
            self.io.send(b'\xfa')
            wait(0.2)
        elif self._clock:
            self._clock[0].set()               # the clock first, then STOP (twice: it must not be lost)
            self._clock[1].join()
            self._clock = None
            for _ in range(2):
                self.io.send(b'\xfc')
                wait(0.05)
            wait(0.25)

    # ---- phases --------------------------------------------------------------
    def intro(self, ask_fw=True):
        c = self.cfg
        self.note('\n== %s, label %s: sending on %r, listening on %s' % (
            VERSION, self.label, self.io.out_name, ' and '.join(repr(p.in_name) for p in self.ports)))
        self.note('   MIDI tracks A-H: input channels %s, out channels %s' % (
            ','.join(str(x + 1) for x in c['in']), ','.join(str(x + 1) for x in c['out'])))
        if ask_fw:
            fw = ask('Firmware on the unit (the .syx file name, or Enter for midi-expression-%s.syx):' % VERSION).strip()
            self.extra['firmware'] = fw or 'midi-expression-%s.syx' % VERSION
            ask('EXPR TEST loaded (docs/TEST-SETUP.md), sequencer stopped. Enter.')
        self.reset_inputs()
        missing = []
        for t in NAMES:
            self.state[t]['VEL'] = 100              # a velocity route takes the note's velocity
            self.send((0x90 | self.tin(t), 60, 100))
            wait(0.08)
            self.send((0x80 | self.tin(t), 60, 0))
            got = [m for _, m in self.settle(0.1) if (m[0] & 0xF0) == 0x90 and m[2]]
            if not any((m[0] & 15) == self.tout(t) for m in got):
                missing.append('%s (in %d, out %d)' % (t, self.tin(t) + 1, self.tout(t) + 1))
        self.check('intro', 'a note into each MIDI track comes out on its channel', not missing,
                   'no note back from ' + ', '.join(missing) + ': check MIDI CONFIG > CHANNELS and each SRC page'
                   if missing else '')
        return not missing

    def tracks(self, n=30):
        """Every source into every track, alone, against the model."""
        P = 'tracks'
        for t in NAMES:
            self.note('\n== track %s' % t)
            marks = self.mark()
            m0 = self.io.mark()
            self.fails = []
            for kind in INPUTS:
                top, lo = (8191, -8192) if kind == 'PB' else (127, 0)
                vals = [lo, top, (lo + top) // 2] + [random.randint(lo, top) for _ in range(n)] + [0]
                for v in vals:
                    self.play(t, kind, v, gap=0)
                    self.compare(t, '%s %s' % (kind, v), self.settle())
            outs = TRACKS[t]['out']
            missing = [k for k in outs if (t, k) not in self.own]
            self.check(P, 'track %s: every output exact (%s)' % (t, ', '.join(outs) or 'none'),
                       not self.fails and not missing,
                       '; '.join(self.fails[:4]) + ('; own value never learned: ' + ', '.join(missing) if missing else ''))
            bad = self.stray(t, self.io.since(m0))
            self.check(P, 'track %s: nothing else on its channel' % t, not bad, ' '.join(bad[:6]))
            others = [hexs(m) for _, m in self.io.since(m0)
                      if self.key(m) and (m[0] & 15) != self.tout(t) and (m[0] & 15) in self.cfg['out']]
            self.check(P, 'track %s: the other tracks stay quiet' % t, not others, ' '.join(others[:6]))
            self.mirrored(P, marks, 'track %s' % t)
        self.extra['own'] = {'%s %s' % k: v for k, v in self.own.items()}

    def notes(self, n=20):
        """Notes into every track: V.SNS on the velocity, velocity routes before the note."""
        P = 'notes'
        self.note('\n== notes: %d per track' % n)
        marks = self.mark()
        for t in NAMES:
            o, vs, fails = self.tout(t), TRACKS[t]['vsens'], []
            vcc = TRACKS[t].get('vel_cc')
            self.fails = []
            for i, v in enumerate([1, 127, 64] + [random.randint(1, 127) for _ in range(n - 3)]):
                note = 48 + i % 40           # C3 and up: clear of any low notes the Digitakt uses itself
                self.state[t]['VEL'] = v
                self.send((0x90 | self.tin(t), note, v), gap=0)
                got = self.settle(0.05)
                self.send((0x80 | self.tin(t), note, 0), gap=0)
                ons = [j for j, (_, m) in enumerate(got) if (m[0] & 0xF0) == 0x90 and (m[0] & 15) == o and m[2]]
                if not ons:
                    fails.append('vel %d: no note' % v)
                    continue
                out = got[ons[0]][1][2]
                if out != sens(v, vs):
                    fails.append('vel %d: out %d, want %d' % (v, out, sens(v, vs)))
                if vcc:
                    ccs = [j for j, (_, m) in enumerate(got) if self.key(m) == (o, vcc)]
                    if ccs and ccs[0] > ons[0]:
                        fails.append('vel %d: %s after its note' % (v, vcc))
                    self.compare(t, 'vel %d' % v, got)
                self.settle(0.01)
            self.check(P, 'track %s: V.SNS %d %%%s' % (t, vs, ', VEL -> %s before each note' % vcc if vcc else ''),
                       not fails and not self.fails, '; '.join((fails + self.fails)[:4]))
        self.mirrored(P, marks, 'notes')

    def multi(self, n=3000):
        P = 'multi'
        self.note('\n== multi: %d random messages to all eight tracks, interleaved' % n)
        marks = self.mark()
        m0 = self.io.mark()
        for _ in range(n):
            t, kind = random.choice(NAMES), random.choice(INPUTS)
            top, lo = (8191, -8192) if kind == 'PB' else (127, 0)
            self.play(t, kind, random.randint(lo, top), gap=0.002)   # within DIN's rate, outputs included
        self.settle(0.6)
        self.fails = []
        for t in NAMES:
            self.compare(t, '%s after the burst' % t)
        self.check(P, 'after the burst every track\'s outputs are the model\'s', not self.fails, '; '.join(self.fails[:6]))
        bad = sum((self.stray(t, self.io.since(m0)) for t in NAMES), [])
        self.check(P, 'nothing a track should not send', not bad, ' '.join(bad[:6]))
        self.mirrored(P, marks, 'multi')
        self.reset_inputs()

    def latency(self, n=40):
        P = 'latency'
        self.note('\n== latency: pitch bend through track A, %d round trips to each listening port' % n)
        res = {p.in_name: [] for p in self.ports}
        for k in range(n):
            v = (k % 2 * 2 - 1) * (500 + k * 37)
            self.fold()
            marks = self.mark()
            self.state['A']['PB'] = v
            t = self.io.send(bytes(self.msg('A', 'PB', v)))
            deadline = time.perf_counter() + 0.2
            pending = set(range(len(self.ports)))
            while pending and time.perf_counter() < deadline:
                for i in list(pending):
                    got = [tt for tt, m in self.ports[i].since(marks[i]) if self.key(m) == (self.tout('A'), 'PB')]
                    if got:
                        res[self.ports[i].in_name].append((got[0] - t) * 1000)
                        pending.discard(i)
                wait(0.001)
            wait(0.03)
        self.fold()
        self.extra.setdefault('latency_ms', {})
        for name, ms in res.items():
            key = '%s -> %s' % (self.io.out_name, name)
            if ms:
                self.extra['latency_ms'][key] = {'n': len(ms), 'median': round(statistics.median(ms), 2), 'max': round(max(ms), 2)}
            self.check(P, '%s: every pitch bend came back (%d of %d)' % (key, len(ms), n), len(ms) == n,
                       'median %.1f ms, max %.1f ms' % (statistics.median(ms), max(ms)) if ms else '')
        self.reset_inputs()

    def seq(self):
        P = 'seq'
        self.note('\n== seq: the sequencer plays track A while its wheel and bend move')
        marks = self.mark()
        m0 = self.io.mark()
        self.transport(True)
        self.fails = []
        for _ in range(80):
            kind = random.choice(('MW', 'PB'))
            self.play('A', kind, random.randint(-4000, 4000) if kind == 'PB' else random.randint(0, 127), gap=0)
            self.compare('A', 'playing', self.settle(0.04))
        self.transport(False)
        self.settle(0.3)
        notes = [m for _, m in self.io.since(m0) if (m[0] & 0xF0) == 0x90 and (m[0] & 15) == self.tout('A') and m[2]]
        self.check(P, 'started over MIDI, the sequencer\'s notes went out (%d)' % len(notes), len(notes) >= 2)
        self.check(P, 'the wheel and bend stayed exact while playing', not self.fails, '; '.join(self.fails[:4]))
        self.mirrored(P, marks, 'seq')
        self.reset_inputs()

    def flood(self, seconds=20):
        P = 'flood'
        self.note('\n== flood: %d s at the MIDI wire rate to all eight tracks, sequencer playing at 160 BPM' % seconds)
        ask('For the next %d seconds watch the screen: turn a knob, change a page or two, see that it keeps up. '
            'Enter to start.' % seconds)
        marks = self.mark()
        m0 = self.io.mark()
        self.transport(True, 160)
        held, count, t_end = {}, 0, time.time() + seconds
        while time.time() < t_end:
            t = random.choice(NAMES)
            r = random.randrange(10)
            if r < 3:
                n = random.randrange(48, 96)
                ch = self.tin(t)
                if (ch, n) in held:
                    self.send((0x80 | ch, n, 0), gap=0)
                    del held[(ch, n)]
                else:
                    v = random.randint(1, 127)
                    self.state[t]['VEL'] = v
                    self.send((0x90 | ch, n, v), gap=0)
                    held[(ch, n)] = 1
            else:
                kind = INPUTS[r % 4]
                top, lo = (8191, -8192) if kind == 'PB' else (127, 0)
                self.play(t, kind, random.randint(lo, top), gap=0)
            count += 1
            wait(0.001)
        for ch, n in list(held):
            self.send((0x80 | ch, n, 0), gap=0.002)
        wait(0.5)                               # the note offs through DIN before STOP
        self.transport(False)
        self.extra['flood'] = {'messages_sent': count, 'seconds': seconds}
        self.settle(0.8)
        self.answer(P, 'During the flood, did the screen and keys keep up (knobs, page changes)?')
        live = {}
        for _, m in self.io.since(m0):
            if (m[0] & 0xF0) in (0x90, 0x80) and len(m) > 2:
                k = (m[0] & 15, m[1])
                live[k] = live.get(k, 0) + (1 if (m[0] & 0xF0) == 0x90 and m[2] else -1)
        stuck = sorted(k for k, v in live.items() if v > 0)
        self.check(P, 'every note on got its note off', not stuck, 'stuck (channel, note): %s' % stuck[:8])
        bad = [hexs(m) for _, m in self.io.since(m0) if m[0] < 0x80 or any(b > 127 for b in m[1:])]
        self.check(P, 'every message well formed', not bad, ' '.join(bad[:5]))
        self.fails = []
        for t in NAMES:
            self.compare(t, '%s after the flood' % t)
        self.check(P, 'after the flood every track\'s outputs are the model\'s', not self.fails, '; '.join(self.fails[:6]))
        self.mirrored(P, marks, 'flood', info=True)
        self.reset_inputs()

    def clockloss(self, n=300):
        """Do live inputs get lost while clock runs into the Digitakt? Each input should get one answer.
        Notes to track H (THRU OFF: only stock passes them) are the stock baseline."""
        P = 'clockloss'
        self.note('\n== clockloss: %d inputs each with the clock stopped, the clock running, the sequencer playing, '
                  'and the clock byte aimed inside a message' % n)
        o_a, o_h = self.tout('A'), self.tout('H')

        def run(label):
            lost_pb, lost_note = 0, 0
            for k in range(n):
                v = (k % 2 * 2 - 1) * (300 + (k * 53) % 7000)
                m = self.io.mark()
                self.play('A', 'PB', v, gap=0)
                note = 48 + k % 48
                self.send((0x90 | self.tin('H'), note, 90), gap=0)
                wait(0.012 + random.random() * 0.02)
                self.send((0x80 | self.tin('H'), note, 0), gap=0)
                wait(0.012)
                got = self.io.since(m)
                if not any(self.key(x) == (o_a, 'PB') for _, x in got):
                    lost_pb += 1
                if not any((x[0] & 0xF0) == 0x90 and (x[0] & 15) == o_h and x[1] == note and x[2] for _, x in got):
                    lost_note += 1
            self.fold()
            self.check(P, '%s: live pitch bends with no answer %d of %d; stock notes lost %d of %d' % (
                label, lost_pb, n, lost_note, n), True if not lost_pb and not lost_note else 'INFO')
            self.extra.setdefault('clockloss', {})['%s via %s' % (label, self.io.out_name)] = {'pb_lost': lost_pb, 'notes_lost': lost_note, 'n': n}
        def aimed(label, first, before=False):
            period = 60 / 160 / 24
            lost_pb, lost_note = 0, 0

            def at(t):
                while True:
                    left = t - time.perf_counter()
                    if left <= 0:
                        return
                    if left > 0.002:
                        time.sleep(left - 0.0015)

            nxt = time.perf_counter() + period
            for k in range(n):
                v = (k % 2 * 2 - 1) * (300 + (k * 53) % 7000)
                note = 48 + k % 48
                pb = self.msg('A', 'PB', v)
                on, off = (0x90 | self.tin('H'), note, 90), (0x80 | self.tin('H'), note, 0)
                m = self.io.mark()
                self.state['A']['PB'] = v
                pair = (pb, on) if first == 'PB' else (on, pb)
                if before:                      # the clock byte, then the pair 0.02 to 0.25 ms later
                    at(nxt)
                    self.io.send(b'\xf8')
                    at(nxt + 0.00002 + random.random() * 0.00023)
                    for x in pair:
                        self.io.send(bytes(x))
                else:                           # the pair, then the clock byte 0.1 to 0.7 ms later
                    at(nxt - (0.0001 + random.random() * 0.0006))
                    for x in pair:
                        self.io.send(bytes(x))
                    at(nxt)
                    self.io.send(b'\xf8')
                nxt += period
                at(nxt - period / 2)
                self.io.send(bytes(off))
                for _ in range(2):              # two more clock ticks, away from anything else
                    at(nxt)
                    self.io.send(b'\xf8')
                    nxt += period
                at(nxt - period / 2)
                got = self.io.since(m)
                if not any(self.key(x) == (o_a, 'PB') for _, x in got):
                    lost_pb += 1
                if not any((x[0] & 0xF0) == 0x90 and (x[0] & 15) == o_h and x[1] == note and x[2] for _, x in got):
                    lost_note += 1
            self.fold()
            self.check(P, '%s: live pitch bends with no answer %d of %d; stock notes lost %d of %d' % (
                label, lost_pb, n, lost_note, n), True if not lost_pb and not lost_note else 'INFO')
            self.extra.setdefault('clockloss', {})['%s via %s' % (label, self.io.out_name)] = {'pb_lost': lost_pb, 'notes_lost': lost_note, 'n': n}

        run('clock stopped')
        stop = threading.Event()

        def tick():
            nxt = time.perf_counter()
            while not stop.is_set():
                self.io.send(b'\xf8')
                nxt += 60 / 160 / 24
                time.sleep(max(0, nxt - time.perf_counter()))
        th = threading.Thread(target=tick, daemon=True)
        th.start()
        wait(0.3)
        run('clock running, sequencer stopped')
        self.io.send(b'\xfa')
        wait(0.2)
        run('clock running, sequencer playing')
        stop.set()
        th.join()
        for _ in range(2):
            self.io.send(b'\xfc')
            wait(0.05)
        wait(0.25)
        # The clock byte on purpose inside a message: sent 0.1 to 0.7 ms after it, so on a DIN
        # cable it lands among the message's bytes (one byte takes 0.32 ms). Once inside the
        # pitch bend (the mod's path), once inside the note (stock's alone).
        aimed('clock byte inside the pitch bend', 'PB')
        aimed('clock byte inside the note', 'note')
        # A clock byte just before a message: the interface's problem, not the Digitakt's
        # (scripts/loopback.py tests it); the ports' guard keeps it out of these runs.
        self.reset_inputs()

    def probe(self, P, when):
        """A short check that EXPR TEST's settings are what they were set to."""
        self.fails = []
        for t, kind in (('B', 'MW'), ('C', 'MW'), ('D', 'AT'), ('F', 'AT'), ('G', 'AT')):
            for v in (100, 0, 37):
                self.play(t, kind, v, gap=0)
                self.compare(t, '%s %s %d' % (t, kind, v), self.settle(0.08))
        m1 = self.io.mark()
        self.play('C', 'PB', 4000)
        self.play('H', 'MW', 90)
        self.settle(0.1)
        leak = self.stray('C', self.io.since(m1)) + self.stray('H', self.io.since(m1))
        if leak:
            self.fails.append('THRU OFF not kept: ' + ' '.join(leak[:3]))
        self.send((0x90 | self.tin('B'), 60, 100), gap=0)
        got = self.settle(0.1)
        self.send((0x80 | self.tin('B'), 60, 0))
        vel = [m[2] for _, m in got if (m[0] & 0xF0) == 0x90 and (m[0] & 15) == self.tout('B') and m[2]]
        if not vel or vel[0] != sens(100, 50):
            self.fails.append('B: V.SNS 50 %% (velocity 100 -> %s)' % vel[:1])
        self.state['B']['VEL'] = 100
        self.reset_inputs()
        self.check(P, 'tracks B, C, D, F, G, H as set (%s)' % when, not self.fails, '; '.join(self.fails[:5]))

    def persist(self):
        P = 'persist'
        self.note('\n== persist: the settings come back with the project and at power-on')
        self.probe(P, 'before')
        ask('Load another project (any), then LOAD PROJECT: EXPR TEST again. Enter.')
        self.probe(P, 'after loading another project and back')
        ask('Turn the Digitakt off, wait five seconds, turn it on (it opens the last project). Enter when it is up.')
        self.probe(P, 'after a power cycle')
        ask('On track B\'s EXPRESSION page set M.DEP to +32.00 (do not save). Then LOAD PROJECT: EXPR TEST again. Enter.')
        self.probe(P, 'after an unsaved change and a reload (the saved +64.00 back)')
        self.answer(P, 'SAVE PROJECT AS a new name (EXPR COPY); on it change track B\'s M.DEP and save; '
                       'LOAD PROJECT: EXPR TEST: track B shows +64.00 again, and EXPR COPY keeps its own value?')
        self.answer(P, 'Delete a different project (one you do not need, or make one): EXPR TEST still loads with its settings?')
        ask('LOAD PROJECT > CREATE NEW (a new, empty project). Enter.')
        m1 = self.io.mark()
        for v in (0, 100):
            self.send(self.msg('B', 'MW', v))
            self.settle(0.1)
        cc74 = [hexs(m) for _, m in self.io.since(m1) if self.key(m) == (self.tout('B'), 'CC74')]
        self.check(P, 'CREATE NEW: track B\'s route to CC 74 is gone (the default MIDI preset)', not cc74, ' '.join(cc74[:4]))
        self.answer(P, 'CREATE NEW: every track\'s EXPRESSION page shows the SELECT DEFAULT preset '
                       '(or the factory settings), not EXPR TEST\'s?')
        ask('LOAD PROJECT: EXPR TEST again. Enter.')
        self.reset_inputs()
        self.probe(P, 'EXPR TEST loaded after CREATE NEW')

    def ears(self):
        P = 'ears'
        ch = 0                                  # audio track 1 on channel 1
        self.note('\n== ears: audio track 1 (BEND 2.00, MW to filter frequency +64, AT to volume -64)')
        ask('Select audio track 1, volume at a comfortable level. Enter; a note for each sweep.')

        def sweep(msgs, gap):
            self.send((0x90 | ch, 60, 100))
            wait(0.3)
            for m in msgs:
                self.send(m, gap=gap)
            wait(0.2)
            self.send((0x80 | ch, 60, 0))
        up = (list(range(8192, 16384, 256)) + [16383] * 40 + list(range(16383, 0, -256)) + [0] * 40
              + list(range(0, 8193, 256)) + [8192] * 20)
        sweep([(0xE0 | ch, min(v, 16383) & 127, min(v, 16383) >> 7) for v in up], 0.025)
        self.answer(P, 'The pitch rose a whole tone, held, fell a whole tone below, held, came back?')
        sweep([(0xB0 | ch, 1, v) for v in list(range(0, 128, 4)) + list(range(127, -1, -4))], 0.03)
        self.answer(P, 'The mod wheel opened the filter and closed it again?')
        sweep([(0xD0 | ch, v) for v in list(range(0, 128, 4)) + list(range(127, -1, -4))], 0.03)
        self.answer(P, 'Aftertouch turned the volume down and back up?')

    def auto(self):
        P = 'auto'
        a = self.cfg['auto']
        self.note('\n== auto: channel %d plays the active track and leaves it on a track change' % (a + 1))
        ask('Track H\'s channel OFF and AUTO CHANNEL %d (docs/TEST-SETUP.md). Select MIDI track A (TRK + trig 9). Enter.'
            % (a + 1))
        o = self.tout('A')
        self.send((0xE0 | a, 0, 64))
        self.settle(0.15)
        own = self.own.get(('A', 'PB'))
        if own is None:
            self.send((0xE0 | a, (8192 + 1000) & 127, (8192 + 1000) >> 7))
            self.settle(0.15)
            h = self.heard.get((o, 'PB'))
            own = h - 1000 if h is not None else None
        self.send((0xE0 | a, (8192 + 2000) & 127, (8192 + 2000) >> 7))
        self.settle(0.15)
        self.check(P, 'the auto channel bends track A',
                   own is not None and self.heard.get((o, 'PB')) == clamp(own + 2000, 0, 16383),
                   'heard %s, own %s' % (self.heard.get((o, 'PB')), own))
        ask('Select MIDI track B (TRK + trig 10); the program keeps the bend held. Enter.')
        self.settle(0.2)
        self.check(P, 'track change: track A\'s pitch bend back to its own (%s)' % own, self.heard.get((o, 'PB')) == own,
                   'heard %s' % self.heard.get((o, 'PB')))
        self.send((0xB0 | a, 1, 33))
        got = [m for _, m in self.settle(0.15) if (m[0] & 15) == self.tout('B') and self.key(m)]
        self.check(P, 'the auto channel now plays track B', bool(got), ' '.join(hexs(m) for m in got[:4]))
        self.send((0xE0 | a, 0, 64), (0xB0 | a, 1, 0))
        self.settle(0.2)
        ask('Set AUTO CHANNEL back to OFF and track H to 16. Enter.')

    def ui(self):
        P = 'ui'
        self.note('\n== ui: the page and the presets, by eye')
        for q in [
            'TRIG, TRIG opens the EXPRESSION page; a tap of TRIG or NO closes it; other page keys work as stock',
            'TRIG held on the EXPRESSION page: after a moment V.SNS and the depths show values (LEV too); letting go keeps the page',
            'TRIG held on the TRIG page shows the stock values, and letting go does not open the EXPRESSION page',
            'BEND\'s box is as large as the destination boxes; BEND turns in fine steps; FUNC + turn snaps to whole semitones',
            'Depths turn finely; FUNC + turn jumps in 16s; destination lists open and close with the knob',
            'Holding knob A for a second opens the preset menu; a twist while holding cancels it',
            'In LOAD / SAVE lists the LEVEL knob moves one row a detent; holding UP or DOWN scrolls',
            'SAVE: FUNC, pick a letter, let go writes it and moves on (type FAST)',
            'SAVE name: holding an arrow repeats; FUNC + NO deletes the letter before the cursor; FUNC + YES inserts a space',
            'SAVE over a used slot asks first; NO goes back without saving',
            'LOAD puts the preset on the current track only; other tracks unchanged',
            'RESET TO DEFAULT asks first and resets only the current track',
            'SELECT DEFAULT then RESET TO DEFAULT: the current track gets the chosen preset',
            'An audio preset is not offered on a MIDI track, and the other way round',
            'A project or preset saved with 0.11 (BEND=5) loads as BEND 5.00',
            'Every audio track 1-8 shows its EXPR TEST settings (docs/TEST-SETUP.md) after the power cycle',
            'MIDI track: MW / AT / VEL to the track\'s own LFO (SPD, DEP ...) moves the LFO audibly on a synth',
            'MIDI track: on the SRC page switch MW off (FUNC + its knob, it shows --): the mod wheel then sends nothing there',
            'The track\'s own LFO and p-locks on PB/MW still play, with the live input added (no jumps)',
        ]:
            self.answer(P, q + '?')

    # ---- report ------------------------------------------------------------------
    def save(self):
        os.makedirs(self.out_dir, exist_ok=True)
        dur = round((time.time() - self.t_start) / 60, 1)
        rep = {'version': VERSION, 'label': self.label,
               'started': time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(self.t_start)), 'minutes': dur,
               'ports': [{'in': p.in_name, 'out': p.out_name} for p in self.ports],
               'channels': {'in': [x + 1 for x in self.cfg['in']], 'out': [x + 1 for x in self.cfg['out']]},
               'results': self.results, **self.extra}
        with open(os.path.join(self.out_dir, 'report.json'), 'w') as fh:
            json.dump(rep, fh, indent=1)
        with open(os.path.join(self.out_dir, 'log.txt'), 'w') as fh:
            fh.write('\n'.join(self.log) + '\n')
        ev = []
        for p in self.ports:
            ev += [(t, 'out ' + p.out_name, m) for t, m in p.tx]      # the clock sent too: its timing
            ev += [(t, 'in ' + p.in_name, m) for t, m in p.rx if m != b'\xf8']
        ev.sort(key=lambda e: e[0])
        t0 = ev[0][0] if ev else 0
        with open(os.path.join(self.out_dir, 'midi.jsonl'), 'w') as fh:
            for t, d, m in ev:
                fh.write(json.dumps({'ms': round((t - t0) * 1000, 2), 'port': d, 'hex': m.hex()}) + '\n')
        p = sum(r['status'] == 'PASS' for r in self.results)
        f = [r for r in self.results if r['status'] == 'FAIL']
        s = sum(r['status'] == 'SKIP' for r in self.results)
        with open(os.path.join(self.out_dir, 'summary.md'), 'w') as fh:
            fh.write('# MIDI Expression %s on a Digitakt mk1, OS 1.54\n\n' % VERSION)
            fh.write('- Run: %s, %s minutes, label `%s`\n' % (rep['started'], dur, self.label))
            fh.write('- Firmware: %s\n' % self.extra.get('firmware', '?'))
            fh.write('- Ports: %s\n' % '; '.join('in `%s`, out `%s`' % (q.in_name, q.out_name) for q in self.ports))
            fh.write('- Result: %d passed, %d failed, %d skipped\n' % (p, len(f), s))
            for k, v in self.extra.get('latency_ms', {}).items():
                fh.write('- Pitch bend round trip %s: median %s ms, max %s ms\n' % (k, v['median'], v['max']))
            fh.write('\n| Sent on | Phase | Check | Result | Note |\n| --- | --- | --- | --- | --- |\n')
            for r in self.results:
                fh.write('| %s | %s | %s | %s | %s |\n' % (r['via'], r['phase'], r['check'].replace('|', '/'),
                                                         r['status'], r['detail'].replace('|', '/')))
        print('\n%d passed, %d failed, %d skipped in %s minutes. Results in %s' % (p, len(f), s, dur, self.out_dir))
        for r in f:
            print('  FAIL %s: %s  %s' % (r['phase'], r['check'], r['detail']))


GROUPS = {'core': ['tracks', 'notes', 'multi', 'latency'], 'play': ['seq', 'clockloss', 'flood'],
          'persist': ['persist'], 'ui': ['ui'], 'ears': ['ears'], 'auto': ['auto']}
GROUPS['all'] = GROUPS['core'] + GROUPS['play'] + GROUPS['persist'] + GROUPS['ui']
SECOND_PASS = ('tracks', 'notes', 'multi', 'latency', 'seq', 'clockloss')     # repeated, sending on the second connection


def main():
    ap = argparse.ArgumentParser(description='Hardware qualification for MIDI Expression.')
    ap.add_argument('--setup', action='store_true', help='print the test project (docs/TEST-SETUP.md)')
    ap.add_argument('--list', action='store_true', help='list the MIDI ports')
    ap.add_argument('--in', dest='inp')
    ap.add_argument('--out')
    ap.add_argument('--in2', help='a second connection to listen on and send from (USB beside DIN)')
    ap.add_argument('--out2')
    ap.add_argument('--label', default='full')
    ap.add_argument('--only', default='all', help='phases or groups, comma separated: ' + ', '.join(GROUPS))
    ap.add_argument('--tracks', default='9,10,11,12,13,14,15,16', help='TRACK A-H CHANNEL (1-16)')
    ap.add_argument('--outs', default='1,2,3,4,5,6,7,8', help='CHAN of MIDI tracks A-H (1-16)')
    ap.add_argument('--auto', type=int, default=16, help='AUTO CHANNEL for the auto phase (1-16)')
    ap.add_argument('--seed', type=int, default=None)
    a = ap.parse_args()
    if a.setup:
        print(open(SETUP_DOC).read())
        return
    if a.list:
        import rtmidi
        print('inputs: ', rtmidi.MidiIn().get_ports())
        print('outputs:', rtmidi.MidiOut().get_ports())
        return
    cfg = {'in': [int(x) - 1 for x in a.tracks.split(',')], 'out': [int(x) - 1 for x in a.outs.split(',')],
           'auto': a.auto - 1}
    phases = []
    for p in a.only.split(','):
        phases += GROUPS.get(p.strip(), [p.strip()])
    seed = a.seed if a.seed is not None else int(time.time())
    random.seed(seed)
    io = Io(a.inp, a.out)
    io2 = Io(a.in2, a.out2 or a.in2) if a.in2 else None
    out_dir = os.path.join(ROOT, 'results', 'hardware', 'qual-%s-%s' % (a.label, time.strftime('%Y%m%d-%H%M%S')))
    run = Qual(io, io2, cfg, out_dir, a.label)
    run.extra['seed'] = seed
    try:
        if run.intro():
            for p in phases:
                getattr(run, p)()
            again = [p for p in phases if p in SECOND_PASS]
            if io2 and again:                       # the same, sent on the second connection
                run.note('\n######## now sending on %s, listening on both' % io2.out_name)
                run.io, run.mirror, run.ports, run.seen, run.heard = io2, io, [io2, io], len(io2.rx), {}
                if run.intro(ask_fw=False):
                    for p in again:
                        getattr(run, p)()
    except KeyboardInterrupt:
        print('\nstopped')
    finally:
        run.save()
        io.close()
        if io2:
            io2.close()


if __name__ == '__main__':
    main()
