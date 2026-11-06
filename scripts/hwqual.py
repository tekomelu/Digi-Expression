#!/usr/bin/env python3
"""Hardware qualification for MIDI Expression (Digitakt mk1, OS 1.54).

One test project, set up once by hand (about ten minutes, printed by
--setup), then mostly automatic phases: the program plays the keyboard,
records everything the Digitakt sends back and checks it against a model of
the firmware's sums (expr.c). Where only your ears or eyes can tell (audio
tracks, the page, presets, saving), it asks a yes/no question; every answer
can take a note.

    computer MIDI OUT  ->  Digitakt MIDI IN   (or its USB port)
    Digitakt MIDI OUT  ->  computer MIDI IN

    python3 -m pip install --user python-rtmidi
    python3 scripts/hwqual.py --setup                 # the test project, step by step
    python3 scripts/hwqual.py --list                  # the MIDI ports
    python3 scripts/hwqual.py --in "Interface" --out "Interface" --label din
    python3 scripts/hwqual.py --in "Digitakt" --out "Digitakt" --label usb --only core

Phases (--only takes a comma list or a group):
  core     thru, routes, merge, multi, latency
  play     seq, flood (the program starts and stops the sequencer itself)
  auto     the auto channel and track changes (you select tracks; not in all)
  persist  power-cycle and reload: the settings come back
  ears     audio tracks, by ear (not in all)
  ui       the page, presets and 0.12's changes, by eye
  all      core, play, persist, ui (the default)

Writes results/hardware/qual-<label>-<date-time>/: report.json (every check,
the timing and your notes), summary.md (for the Modwerk hardware report),
log.txt, midi.jsonl (every message both ways, timestamped). Ctrl+C stops and
still saves. Nothing here sends SysEx or writes to the Digitakt's memory.
"""
import argparse
import json
import os
import random
import statistics
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from hwtest import Io, ask, hexs, wait, ROOT  # noqa: E402

VERSION = '0.12.3-experimental'
SETTLE = 0.06                   # the Digitakt answers in about 4..40 ms

SETUP = '''
THE TEST PROJECT (once; save it as EXPR TEST and keep it)

Start from an empty project.

SETTINGS > MIDI CONFIG
  SYNC:      CLOCK RECEIVE on, TRANSPORT RECEIVE on (the program starts and stops
             the sequencer and sets its tempo itself), CLOCK SEND as you like.
  PORT CONFIG: INPUT FROM  MIDI+USB, OUTPUT TO  MIDI+USB,
             RECEIVE NOTES on, RECEIVE CC/NRPN on.
  CHANNELS:  AUTO CHANNEL {auto}; TRACK 1 = {audio};
             TRACK A = {ta}, TRACK B = {tb}, TRACK C = {tc}; other tracks off.

MIDI track A (TRK + trig 9): SRC page CHAN on, channel {oa}.
  Four trigs on steps 1, 5, 9, 13 (note C4, velocity 100).
  EXPRESSION page (TRIG, TRIG): the defaults (THRU ON, V.SNS 100 %, no routes).

MIDI track B (TRK + trig 10): CHAN on, channel {ob}.
  AMP page: SEL1 = 74.
  EXPRESSION: THRU ON, V.SNS 50 %,
    MW -> CC:CC1 VAL (shows #74), M.DEP +64.00
    VEL -> SRC:BC,               V.DEP +32.00
    AT -> SRC:PITCH BEND,        A.DEP +32.00

MIDI track C (TRK + trig 11): CHAN on, channel {oc}.
  AMP page: SEL2 = 1 (the mod wheel's CC). FLTR page: VAL2 = 100.
  EXPRESSION: THRU OFF, V.SNS 100 %,
    MW -> CC:CC2 VAL (shows #1), M.DEP -32.00

On tracks B and C make sure CC VAL1 / CC VAL2 (FLTR page) are switched on (a value, not --).

Audio track 1: a sample of 3 seconds or more (a pad), or any bright sample with LOOP on;
  AMP decay long, FLTR frequency low.
  EXPRESSION: BEND 2.00, V.SNS 100 %,
    MW -> FLTR:FREQUENCY, M.DEP +64.00 (set the filter low so you hear it open)
    AT -> AMP:VOLUME,     A.DEP -64.00

Save the project (SAVE PROJECT AS: EXPR TEST). Select MIDI track A.
'''


def tdiv(a, b):
    """C integer division (toward zero), as the firmware computes."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def round256(v):
    return (v + 128) >> 8 if v >= 0 else -((-v + 128) >> 8)


def route(src, amount):
    """A route's part in 1/256 of a MIDI step: expr.c live_parts."""
    return tdiv(src * amount, 64)


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def sens(v, s):
    """V.SNS on a velocity: audio.c me_velocity_scaled."""
    if s >= 100 or v <= 0:
        return v
    return clamp(v + ((127 - v) * (100 - s) + 50) // 100, 1, 127)


D = 256                                 # depths in 1/256: 64.00 = 64 * D


class Qual:
    def __init__(self, io, cfg, out_dir, label):
        self.io, self.cfg, self.out_dir, self.label = io, cfg, out_dir, label
        self.results, self.log, self.extra = [], [], {}
        self.t_start = time.time()
        self.heard = {}                 # (out channel, key) -> last value the synth heard
        self.own = {}                   # (out channel, key) -> the track's own value, as learned
        self.seen = 0                   # rx messages already folded into heard
        import threading
        self._send_lock = threading.Lock()
        raw = io.send
        def locked(data, raw=raw):
            with self._send_lock:
                return raw(data)
        io.send = locked                # the clock thread and the program share the port
        self._clock = None

    # ---- bookkeeping ---------------------------------------------------------
    def note(self, text):
        print(text)
        self.log.append(text)

    def check(self, phase, name, ok, detail=''):
        status = 'PASS' if ok is True else 'FAIL' if ok is False else str(ok)
        self.note('  %-5s %s%s' % (status, name, ('  (' + detail + ')') if detail else ''))
        self.results.append({'phase': phase, 'check': name, 'status': status, 'detail': detail,
                             'label': self.label, 't': round(time.time() - self.t_start, 1)})

    def answer(self, phase, question):
        """A yes/no/skip question; any answer may carry a note after a space."""
        while True:
            a = ask(question + '  [y / n / s(kip)] (a note may follow)').strip()
            if a[:1].lower() in ('y', 'n', 's'):
                k, rest = a[:1].lower(), a[1:].strip()
                st = {'y': True, 'n': False, 's': 'SKIP'}[k]
                self.check(phase, question, st, rest)
                return st

    # ---- MIDI ----------------------------------------------------------------
    def send(self, *msgs, gap=0.002):
        for m in msgs:
            self.io.send(bytes(m))
            if gap:
                wait(gap)

    def fold(self):
        """Fold everything received since last time into heard; returns the new messages."""
        with self.io.lock:
            new = self.io.rx[self.seen:]
            self.seen += len(new)
        got = [(t, m) for t, m in new if m[0] not in (0xF8, 0xFE)]
        for _, m in got:
            k = self.key(m)
            if k:
                self.heard[k] = self.value(m)
        return got

    @staticmethod
    def key(m):
        s = m[0] & 0xF0
        ch = m[0] & 15
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

    def transport(self, run, bpm=160):
        """Start (clock at bpm, then START) or stop (STOP, then no clock) the sequencer over MIDI."""
        import threading
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
            self.io.send(b'\xfc')
            wait(0.3)
            self._clock[0].set()
            self._clock[1].join()
            self._clock = None

    def settle(self, t=SETTLE):
        wait(t)
        return self.fold()

    def pb(self, ch, v): return (0xE0 | ch, v & 127, v >> 7)
    def at(self, ch, v): return (0xD0 | ch, v)
    def cc(self, ch, n, v): return (0xB0 | ch, n, v)
    def on(self, ch, n, v): return (0x90 | ch, n, v)
    def off(self, ch, n): return (0x80 | ch, n, 0)

    def learn(self, phase, out, key, send_fn, live_of, probes):
        """The track's own value: send a live value, read what comes out, take
        the live part away. Tries the probes in turn until one comes out unclamped."""
        lo, hi = (0, 16383) if key == 'PB' else (0, 127)
        for p in probes:
            self.fold()
            mark = self.io.mark()
            send_fn(p)
            got = [m for _, m in self.settle(0.12) if self.key(m) == (out, key)]
            if got:
                v = self.value(got[-1])
                if lo < v < hi:
                    own = v - live_of(p)
                    self.own[(out, key)] = own
                    self.note('  own %s on channel %d: %d' % (key, out + 1, own))
                    return own
        self.check(phase, 'own %s on channel %d learned' % (key, out + 1), False, 'no unclamped answer')
        return None

    def expect(self, phase, out, key, want, label):
        """After a settle: is what the synth last heard the model's value?"""
        have = self.heard.get((out, key))
        ok = have == want
        if not ok:
            self.fails.append('%s: want %s, heard %s' % (label, want, have))
        return ok

    def sweep(self, phase, name, out, key, values, send_fn, model):
        """Send each value, check what the synth heard; one check line per sweep."""
        self.fails = []
        for v in values:
            send_fn(v)
            self.settle()
            self.expect(phase, out, key, model(v), '%s %s' % (name, v))
        self.check(phase, '%s: %d values' % (name, len(values)), not self.fails, '; '.join(self.fails[:4]))

    def quiet(self, phase, chans, mark, kinds, name):
        bad = [hexs(m) for _, m in self.io.since(mark)
               if (m[0] & 15) in chans and (m[0] & 0xF0) in kinds]
        self.check(phase, name, not bad, ' '.join(bad[:6]))

    def valid(self, phase, mark):
        msgs = self.io.since(mark)
        bad = [hexs(m) for _, m in msgs if m[0] < 0x80 or any(b > 127 for b in m[1:])]
        self.check(phase, 'every message well formed (%d)' % len(msgs), not bad, ' '.join(bad[:5]))

    def reset_inputs(self):
        for ch in (self.cfg['ta'], self.cfg['tb'], self.cfg['tc'], self.cfg['auto']):
            self.send(self.pb(ch, 8192), self.at(ch, 0), self.cc(ch, 1, 0), self.cc(ch, 2, 0))
        self.settle(0.2)

    # ---- phases --------------------------------------------------------------
    def intro(self):
        c = self.cfg
        self.note('\n== %s, label %s, ports in %r out %r' % (VERSION, self.label, self.io.in_name, self.io.out_name))
        self.note('   channels (1-16): auto %d, input A/B/C %d/%d/%d, out A/B/C %d/%d/%d, audio track 1 on %d' % (
            c['auto'] + 1, c['ta'] + 1, c['tb'] + 1, c['tc'] + 1, c['oa'] + 1, c['ob'] + 1, c['oc'] + 1, c['audio'] + 1))
        fw = ask('Firmware on the unit (the .syx file name, or Enter for %s):' % VERSION).strip()
        self.extra['firmware'] = fw or 'midi-expression-%s.syx' % VERSION
        ask('The EXPR TEST project loaded, sequencer stopped, MIDI track A selected. Enter.')
        mark = self.io.mark()
        self.send(self.on(c['ta'], 60, 100))
        wait(0.15)
        self.send(self.off(c['ta'], 60))
        got = [m for _, m in self.settle(0.15) if (m[0] & 0xF0) in (0x90, 0x80)]
        ok = any((m[0] & 15) == c['oa'] and m[0] & 0xF0 == 0x90 and m[1] == 60 for m in got)
        self.check('intro', 'a note on track A\'s channel comes out on channel %d' % (c['oa'] + 1), ok,
                   ' '.join(hexs(m) for m in got[:4]) or 'nothing came back: check the cables and channels')
        return ok

    def thru(self):
        """Track A: THRU ON, nothing routed: the input adds to the track's own values."""
        c, P = self.cfg, 'thru'
        self.note('\n== thru: track A passes PB, AT, MW and BC, added to its own values')
        i, o = c['ta'], c['oa']
        m0 = self.io.mark()
        own = self.learn(P, o, 'PB', lambda v: self.send(self.pb(i, v)), lambda v: v - 8192, (8192 + 1000, 8192 - 1000, 8192 + 3000))
        if own is not None:
            vals = [8192, 0, 16383, 4096, 12288, 8191, 8193] + [random.randrange(16384) for _ in range(40)]
            self.sweep(P, 'PB thru', o, 'PB', vals, lambda v: self.send(self.pb(i, v)), lambda v: clamp(own + v - 8192, 0, 16383))
        for key, sendf in (('AT', lambda v: self.send(self.at(i, v))),
                           ('CC1', lambda v: self.send(self.cc(i, 1, v))),
                           ('CC2', lambda v: self.send(self.cc(i, 2, v)))):
            own = self.learn(P, o, key, sendf, lambda v: v, (20, 40, 5))
            if own is not None:
                vals = [0, 127, 64, 1, 126] + [random.randrange(128) for _ in range(40)]
                self.sweep(P, key + ' thru', o, key, vals, sendf, lambda v, own=own: clamp(own + v, 0, 127))
        self.valid(P, m0)
        self.quiet(P, (c['ob'], c['oc']), m0, (0xB0, 0xD0, 0xE0), 'nothing on tracks B and C\'s channels')
        self.reset_inputs()

    def routes(self):
        """Track B: MW -> CC VAL1 on CC 74 +64, AT -> PB +32 (and AT thru), VEL -> BC +32, V.SNS 50 %."""
        c, P = self.cfg, 'routes'
        self.note('\n== routes: track B (MW to CC 74, AT to pitch bend, velocity to BC, V.SNS 50 %)')
        i, o = c['tb'], c['ob']
        m0 = self.io.mark()
        mw = lambda v: self.send(self.cc(i, 1, v))
        own74 = self.learn(P, o, 'CC74', mw, lambda v: round256(route(v, 64 * D)), (20, 40, 5))
        if own74 is not None:
            vals = [0, 127, 64, 1] + [random.randrange(128) for _ in range(40)]
            self.sweep(P, 'MW -> CC 74 +64.00', o, 'CC74', vals, mw, lambda v: clamp(own74 + round256(route(v, 64 * D)), 0, 127))
        at = lambda v: self.send(self.at(i, v))
        ownpb = self.learn(P, o, 'PB', at, lambda v: tdiv(route(v, 32 * D) * 16383, 32512), (20, 40, 60))
        if ownpb is not None:
            vals = [0, 127, 64, 1] + [random.randrange(128) for _ in range(30)]
            self.sweep(P, 'AT -> PB +32.00', o, 'PB', vals, at,
                       lambda v: clamp(ownpb + tdiv(route(v, 32 * D) * 16383, 32512), 0, 16383))
        ownat = self.own.get((o, 'AT'))
        if ownat is None:
            ownat = self.learn(P, o, 'AT', at, lambda v: v, (21, 41, 6))
        if ownat is not None:
            vals = [0, 127, 50] + [random.randrange(128) for _ in range(20)]
            self.sweep(P, 'AT thru beside its route', o, 'AT', vals, at, lambda v: clamp(ownat + v, 0, 127))
        # velocity: V.SNS 50 % on the note, VEL -> BC +32 before it
        fails, ownbc = [], None
        for n, v in enumerate([100, 1, 127, 64, 30] + [random.randrange(1, 128) for _ in range(30)]):
            mark = self.io.mark()
            self.send(self.on(i, 48 + n % 24, v), gap=0)
            got = self.settle(0.08)
            self.send(self.off(i, 48 + n % 24))
            notes = [k for k, (_, m) in enumerate(got) if (m[0] & 0xF0) == 0x90 and (m[0] & 15) == o and m[2]]
            bcs = [k for k, (_, m) in enumerate(got) if self.key(m) == (o, 'CC2')]
            if not notes:
                fails.append('vel %d: no note out' % v)
                continue
            vel = got[notes[0]][1][2]
            if vel != sens(v, 50):
                fails.append('vel %d: out %d, want %d' % (v, vel, sens(v, 50)))
            part = round256(route(v, 32 * D))
            if bcs:
                if bcs[0] > notes[0]:
                    fails.append('vel %d: BC after its note' % v)
                bc = got[bcs[-1]][1][2]
                if ownbc is None and 0 < bc < 127:
                    ownbc = bc - part
                elif ownbc is not None and bc != clamp(ownbc + part, 0, 127):
                    fails.append('vel %d: BC %d, want %d' % (v, bc, clamp(ownbc + part, 0, 127)))
            elif ownbc is not None and self.heard.get((o, 'CC2')) != clamp(ownbc + part, 0, 127):
                fails.append('vel %d: no BC (heard %s)' % (v, self.heard.get((o, 'CC2'))))
            self.settle(0.02)
        self.check(P, 'notes: V.SNS 50 %% on the velocity, VEL -> BC +32.00 before each note (own BC %s)' % ownbc,
                   not fails, '; '.join(fails[:4]))
        self.valid(P, m0)
        self.quiet(P, (c['oa'], c['oc']), m0, (0xB0, 0xD0, 0xE0, 0x90), 'nothing on tracks A and C\'s channels')
        self.reset_inputs()

    def merge(self):
        """Track C: THRU OFF, MW -> CC VAL2 on CC 1 -32: one CC 1, the wheel takes away."""
        c, P = self.cfg, 'merge'
        self.note('\n== merge: track C (THRU OFF; MW to CC VAL2 on CC 1, -32.00)')
        i, o = c['tc'], c['oc']
        m0 = self.io.mark()
        mw = lambda v: self.send(self.cc(i, 1, v))
        own1 = self.learn(P, o, 'CC1', mw, lambda v: round256(route(v, -32 * D)), (10, 30, 50, 90))
        if own1 is None:
            self.note('  (track C\'s own CC 1 is 0 or near it: set its FLTR page VAL2 to 100, as the setup says)')
        if own1 is not None:
            vals = [0, 127, 64, 2] + [random.randrange(128) for _ in range(40)]
            self.sweep(P, 'MW -> CC 1 -32.00, merged with the track\'s own CC 1', o, 'CC1', vals, mw,
                       lambda v: clamp(own1 + round256(route(v, -32 * D)), 0, 127))
        mark = self.io.mark()
        self.send(self.pb(i, 12000), self.at(i, 90), self.cc(i, 2, 90))
        self.settle(0.15)
        self.quiet(P, (o,), mark, (0xE0, 0xD0), 'THRU OFF: pitch bend and aftertouch do not pass')
        cc2 = [hexs(m) for _, m in self.io.since(mark) if self.key(m) == (o, 'CC2')]
        self.check(P, 'THRU OFF: breath does not pass', not cc2, ' '.join(cc2[:4]))
        self.valid(P, m0)
        self.reset_inputs()

    def multi(self, n=1500):
        """All three tracks at once, random order, against the model."""
        c, P = self.cfg, 'multi'
        self.note('\n== multi: %d random messages to tracks A, B and C interleaved' % n)
        need = [(c['oa'], 'PB'), (c['oa'], 'CC1'), (c['ob'], 'CC74'), (c['oc'], 'CC1')]
        if any(k not in self.own for k in need):
            self.check(P, 'own values known from thru, routes and merge', 'SKIP', 'run core first')
            return
        state = {'pa': 8192, 'wa': 0, 'wb': 0, 'wc': 0}
        m0 = self.io.mark()
        for _ in range(n):
            r = random.randrange(4)
            if r == 0:
                state['pa'] = random.randrange(16384)
                self.send(self.pb(c['ta'], state['pa']), gap=0.001)
            else:
                k = 'wa wb wc'.split()[r - 1]
                state[k] = random.randrange(128)
                self.send(self.cc((c['ta'], c['tb'], c['tc'])[r - 1], 1, state[k]), gap=0.001)
        self.settle(0.4)
        self.fails = []
        self.expect(P, c['oa'], 'PB', clamp(self.own[(c['oa'], 'PB')] + state['pa'] - 8192, 0, 16383), 'A PB')
        self.expect(P, c['oa'], 'CC1', clamp(self.own[(c['oa'], 'CC1')] + state['wa'], 0, 127), 'A CC1')
        self.expect(P, c['ob'], 'CC74', clamp(self.own[(c['ob'], 'CC74')] + round256(route(state['wb'], 64 * D)), 0, 127), 'B CC74')
        self.expect(P, c['oc'], 'CC1', clamp(self.own[(c['oc'], 'CC1')] + round256(route(state['wc'], -32 * D)), 0, 127), 'C CC1')
        self.check(P, 'after the burst each synth heard the model\'s values', not self.fails, '; '.join(self.fails))
        self.valid(P, m0)
        outs = (c['oa'], c['ob'], c['oc'])
        stray = [hexs(m) for _, m in self.io.since(m0) if (m[0] & 15) not in outs and m[0] < 0xF0]
        self.check(P, 'nothing on other channels', not stray, ' '.join(stray[:6]))
        self.reset_inputs()

    def latency(self, n=40):
        c, P = self.cfg, 'latency'
        self.note('\n== latency: pitch bend through track A, %d round trips' % n)
        ms = []
        for k in range(n):
            v = 8192 + (k % 2 * 2 - 1) * (500 + k * 37)
            self.fold()
            mark = self.io.mark()
            t = self.io.send(bytes(self.pb(c['ta'], v)))
            for _ in range(60):
                got = [(tt, m) for tt, m in self.io.since(mark) if self.key(m) == (c['oa'], 'PB')]
                if got:
                    ms.append((got[0][0] - t) * 1000)
                    break
                wait(0.002)
            wait(0.03)
        self.fold()
        if ms:
            self.extra['latency_ms'] = {'n': len(ms), 'median': round(statistics.median(ms), 2), 'max': round(max(ms), 2)}
        self.check(P, 'every pitch bend came back (%d of %d)' % (len(ms), n), len(ms) == n,
                   'median %.1f ms, max %.1f ms (interface included)' % (statistics.median(ms), max(ms)) if ms else '')
        self.reset_inputs()

    def auto(self):
        """The auto channel follows the active track; the old track gets its own value back."""
        c, P = self.cfg, 'auto'
        self.note('\n== auto: the auto channel goes to the active track, and leaves it on a track change')
        ask('Select MIDI track A (TRK + trig 9). Enter.')
        a, o = c['auto'], c['oa']
        own = self.own.get((o, 'PB'))
        if own is None:
            own = self.learn(P, o, 'PB', lambda v: self.send(self.pb(a, v)), lambda v: v - 8192, (9192, 7192))
        self.send(self.pb(a, 8192 + 2000))
        self.settle(0.15)
        self.check(P, 'auto channel: track A bends', self.heard.get((o, 'PB')) == clamp((own or 0) + 2000, 0, 16383),
                   'heard %s' % self.heard.get((o, 'PB')))
        ask('Select MIDI track B (TRK + trig 10). Keep the bend held (the program does). Enter.')
        self.settle(0.2)
        self.check(P, 'track change: track A\'s pitch bend back to its own (%s)' % own,
                   self.heard.get((o, 'PB')) == own, 'heard %s' % self.heard.get((o, 'PB')))
        self.send(self.pb(a, 8192 + 2000))
        self.settle(0.15)
        mark = self.io.mark()
        self.send(self.cc(a, 1, 33))
        got = [m for _, m in self.settle(0.15) if (m[0] & 15) == c['ob'] and self.key(m)]
        self.check(P, 'the auto channel now plays track B', bool(got), ' '.join(hexs(m) for m in got[:4]))
        self.reset_inputs()
        ask('Select MIDI track A again. Enter.')

    def seq(self):
        c, P = self.cfg, 'seq'
        self.note('\n== seq: the sequencer plays track A while the wheel and bend move')
        m0 = self.io.mark()
        self.transport(True)
        own1 = self.own.get((c['oa'], 'CC1'))
        self.fails = []
        for k in range(60):
            w = random.randrange(128)
            self.send(self.cc(c['ta'], 1, w))
            self.settle(0.05)
            if own1 is not None:
                self.expect(P, c['oa'], 'CC1', clamp(own1 + w, 0, 127), 'MW %d' % w)
        self.transport(False)
        self.settle(0.2)
        notes = [m for _, m in self.io.since(m0) if (m[0] & 0xF0) == 0x90 and (m[0] & 15) == c['oa'] and m[2]]
        self.check(P, 'the sequencer\'s notes went out (%d)' % len(notes), len(notes) >= 2)
        self.check(P, 'the wheel stayed exact while playing', not self.fails, '; '.join(self.fails[:4]))
        self.reset_inputs()

    def flood(self, seconds=20):
        c, P = self.cfg, 'flood'
        self.note('\n== flood: %d s at the MIDI wire rate to tracks A, B and C, clock included, sequencer playing' % seconds)
        ask('For the next %d seconds watch the screen: turn a knob, change a page or two, see that it keeps up. '
            'Enter to start (the program starts the sequencer).' % seconds)
        m0 = self.io.mark()
        self.transport(True, 160)
        sent_on, held = {}, {}
        tin = (c['ta'], c['tb'], c['tc'])
        t_end = time.time() + seconds
        count = 0
        while time.time() < t_end:
            ch = random.choice(tin)
            r = random.randrange(10)
            if r < 3:
                n = random.randrange(36, 84)
                if (ch, n) in held:
                    self.io.send(bytes(self.off(ch, n)))
                    del held[(ch, n)]
                else:
                    self.io.send(bytes(self.on(ch, n, random.randrange(1, 128))))
                    held[(ch, n)] = 1
                    sent_on[ch] = sent_on.get(ch, 0) + 1
            elif r < 5:
                self.io.send(bytes(self.pb(ch, random.randrange(16384))))
            elif r < 7:
                self.io.send(bytes(self.at(ch, random.randrange(128))))
            else:
                self.io.send(bytes(self.cc(ch, random.choice((1, 2)), random.randrange(128))))
            count += 1
            wait(0.001)                 # about 1000 messages a second, the wire's rate
        for ch, n in list(held):
            self.io.send(bytes(self.off(ch, n)))
        self.extra['flood'] = {'messages_sent': count, 'seconds': seconds}
        self.settle(0.5)
        self.transport(False)
        self.settle(0.5)
        self.answer(P, 'During the flood, did the screen and keys keep up (knobs, page changes)?')
        self.valid(P, m0)
        live = {}
        outs = {c['ta']: c['oa'], c['tb']: c['ob'], c['tc']: c['oc']}
        for _, m in self.io.since(m0):
            if (m[0] & 0xF0) in (0x90, 0x80) and len(m) > 2:
                k = (m[0] & 15, m[1])
                live[k] = live.get(k, 0) + (1 if (m[0] & 0xF0) == 0x90 and m[2] else -1)
        stuck = sorted(k for k, v in live.items() if v > 0)
        self.check(P, 'every note on got its note off', not stuck, 'stuck (channel, note): %s' % stuck[:8])
        for ch_in, ch_out in outs.items():
            got = sum(1 for _, m in self.io.since(m0) if (m[0] & 0xF0) == 0x90 and (m[0] & 15) == ch_out and m[2])
            self.note('  info  channel %d: %d note-ons sent, %d came out (the sequencer\'s own included on A)' % (
                ch_in + 1, sent_on.get(ch_in, 0), got))
        self.reset_inputs()
        # the model holds again once the flood is over
        self.fails = []
        for key, sendf, mdl in (('CC74', lambda v: self.send(self.cc(c['tb'], 1, v)), lambda v: round256(route(v, 64 * D))),
                                ('CC1', lambda v: self.send(self.cc(c['tc'], 1, v)), lambda v: round256(route(v, -32 * D)))):
            out = c['ob'] if key == 'CC74' else c['oc']
            own = self.own.get((out, key))
            if own is None:
                continue
            for v in (127, 0, 77):
                sendf(v)
                self.settle()
                self.expect(P, out, key, clamp(own + mdl(v), 0, 127), '%s after the flood, %d' % (key, v))
        self.check(P, 'after the flood the routes are exact again', not self.fails, '; '.join(self.fails[:4]))

    def probe(self, P, when):
        """A short check that the test project's settings are what they were set to."""
        c = self.cfg
        self.note('  probe %s' % when)
        self.fails = []
        res = {}
        for name, ch_in, out, key, depth, own_key in (('B: MW -> CC 74 +64', c['tb'], c['ob'], 'CC74', 64, (c['ob'], 'CC74')),
                                                      ('C: MW -> CC 1 -32', c['tc'], c['oc'], 'CC1', -32, (c['oc'], 'CC1'))):
            vals = {}
            for v in (0, 100, 0):
                self.send(self.cc(ch_in, 1, v))
                self.settle(0.1)
                vals[v] = self.heard.get((out, key))
            got = None if vals.get(100) is None or vals.get(0) is None else vals[100] - vals[0]
            want = round256(route(100, depth * D))
            res[name] = got
            if got is None or abs(got - want) > 1 and not (vals[100] in (0, 127)):
                self.fails.append('%s: step %s, want %s' % (name, got, want))
        self.send(self.on(c['tb'], 60, 100), gap=0)
        got = self.settle(0.1)
        self.send(self.off(c['tb'], 60))
        vel = [m[2] for _, m in got if (m[0] & 0xF0) == 0x90 and (m[0] & 15) == c['ob'] and m[2]]
        if not vel or vel[0] != sens(100, 50):
            self.fails.append('B: V.SNS 50 %% (velocity 100 -> %s, want %d)' % (vel[:1], sens(100, 50)))
        mark = self.io.mark()
        self.send(self.pb(c['tc'], 12000))
        self.settle(0.1)
        if [m for _, m in self.io.since(mark) if self.key(m) == (c['oc'], 'PB')]:
            self.fails.append('C: THRU OFF not kept (pitch bend passed)')
        self.reset_inputs()
        self.check(P, 'track B and C settings as set (%s)' % when, not self.fails, '; '.join(self.fails) or str(res))

    def persist(self):
        P = 'persist'
        self.note('\n== persist: the settings come back with the project and at power-on')
        self.probe(P, 'before')
        ask('Load another project (any), then LOAD PROJECT: EXPR TEST again. Enter.')
        self.probe(P, 'after loading another project and back')
        ask('Turn the Digitakt off, wait five seconds, turn it on (it opens the last project). Enter when it is up.')
        self.probe(P, 'after a power cycle')
        ask('On track B\'s EXPRESSION page set M.DEP to +32.00 (do not save). Then LOAD PROJECT: EXPR TEST again '
            '(it reloads the saved one). Enter.')
        self.probe(P, 'after an unsaved change and a reload (the saved +64 comes back)')
        self.answer(P, 'SAVE PROJECT AS a new name (EXPR COPY); on it change track B\'s M.DEP and save; '
                       'LOAD PROJECT: EXPR TEST: track B shows +64.00 again, and EXPR COPY keeps its own value?')
        self.answer(P, 'Delete a different project (one you do not need, or make one): EXPR TEST still loads with its settings?')
        ask('LOAD PROJECT > CREATE NEW (a new, empty project). Enter.')
        c = self.cfg
        m1 = self.io.mark()
        for v in (0, 100):
            self.send(self.cc(c['tb'], 1, v))
            self.settle(0.1)
        cc74 = [m for _, m in self.io.since(m1) if self.key(m) == (c['ob'], 'CC74')]
        self.check(P, 'CREATE NEW: track B\'s routing to CC 74 is gone (the default MIDI preset)', not cc74,
                   ' '.join(hexs(m) for m in cc74[:4]))
        self.answer(P, 'CREATE NEW: every audio track\'s EXPRESSION page shows the SELECT DEFAULT preset '
                       '(or the factory settings if none is selected), not the last project\'s?')
        ask('LOAD PROJECT: EXPR TEST again, select MIDI track A. Enter.')
        self.reset_inputs()

    def ears(self):
        c, P = self.cfg, 'ears'
        self.note('\n== ears: audio track 1 (BEND 2.00, MW to filter frequency +64, AT to volume -64)')
        ch = c['audio']
        ask('Select audio track 1, volume at a comfortable level. Enter; the program plays a note for each sweep.')
        def sweep(msgs, gap):
            self.send(self.on(ch, 60, 100))         # a fresh note per sweep: an unlooped sample has its length
            wait(0.3)
            for m in msgs:
                self.send(m, gap=gap)
            wait(0.2)
            self.send(self.off(ch, 60))
        up = (list(range(8192, 16384, 256)) + [16383] * 40 + list(range(16383, 0, -256)) + [0] * 40
              + list(range(0, 8193, 256)) + [8192] * 20)
        self.note('  pitch bend: slowly up a whole tone, a second there, down to a whole tone below, a second, back')
        sweep([self.pb(ch, min(v, 16383)) for v in up], 0.025)
        self.answer(P, 'The pitch rose about a whole tone (2 semitones), fell a whole tone below, came back?')
        sweep([self.cc(ch, 1, v) for v in list(range(0, 128, 4)) + list(range(127, -1, -4))], 0.03)
        self.answer(P, 'The mod wheel opened the filter and closed it again?')
        sweep([self.at(ch, v) for v in list(range(0, 128, 4)) + list(range(127, -1, -4))], 0.03)
        self.answer(P, 'Aftertouch turned the volume down and back up?')
        self.reset_inputs()
        ask('On audio track 1\'s EXPRESSION page set V.SNS to 0 %. Enter; the program plays soft and loud notes.')
        for v in (10, 127, 10, 127):
            self.send(self.on(ch, 60, v))
            wait(0.35)
            self.send(self.off(ch, 60))
            wait(0.2)
        self.answer(P, 'With V.SNS 0 %, soft and loud notes sounded the same level?')
        ask('Set V.SNS back to 100 %. Enter.')

    def ui(self):
        P = 'ui'
        self.note('\n== ui: the page, presets and 0.12\'s changes (by eye)')
        q = [
            'TRIG, TRIG opens the EXPRESSION page; a tap of TRIG or NO closes it; other page keys work as stock',
            'TRIG held on the EXPRESSION page: after a moment V.SNS and the depths show values (LEV too), as on the stock pages; letting go keeps the page',
            'TRIG held on the TRIG page shows the stock values, and letting go does not open the EXPRESSION page',
            'BEND turns in fine steps (2.00, 2.01 ...) and its box shows tenths (2.5)',
            'FUNC + turn on BEND snaps to whole semitones OFF, 1, 2 ... 12',
            'Depths turn finely; FUNC + turn jumps in 16s; destination lists open and close with the knob',
            'Holding knob A for a second opens the preset menu; a twist while holding cancels it',
            'In LOAD / SAVE lists the LEVEL knob moves one row a detent',
            'SAVE: FUNC, pick a letter, let go writes it and moves on (type FAST)',
            'SAVE name: holding an arrow repeats; FUNC + NO deletes the letter before the cursor; FUNC + YES inserts a space, as SAVE PROJECT AS',
            'Holding UP or DOWN scrolls the LOAD / SAVE lists',
            'SAVE over a used slot asks first; NO goes back without saving',
            'LOAD puts the preset on the current track only; other tracks unchanged',
            'RESET TO DEFAULT asks first and resets only the current track',
            'SELECT DEFAULT then RESET TO DEFAULT: the current track gets the chosen preset',
            'An audio preset is not offered on a MIDI track, and the other way round',
            'A project or preset saved with 0.11 (BEND=5) loads as BEND 5.00',
            'MIDI track: MW / AT / VEL to the track\'s own LFO (SPD, DEP ...) moves the LFO audibly on the synth',
            'MIDI track: on the SRC page switch MW off (FUNC + its knob, it shows --): the mod wheel then sends nothing on that track',
            'The track\'s own LFO and p-locks on PB/MW still play, with the live input added (no jumps)',
        ]
        for t in q:
            self.answer(P, t + '?')

    # ---- report ------------------------------------------------------------------
    def save(self):
        os.makedirs(self.out_dir, exist_ok=True)
        dur = round((time.time() - self.t_start) / 60, 1)
        rep = {'version': VERSION, 'label': self.label, 'started': time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(self.t_start)),
               'minutes': dur, 'ports': {'in': self.io.in_name, 'out': self.io.out_name},
               'channels': {k: v + 1 for k, v in self.cfg.items()},
               'own': {'%d %s' % (k[0] + 1, k[1]): v for k, v in self.own.items()},
               'results': self.results, **self.extra}
        with open(os.path.join(self.out_dir, 'report.json'), 'w') as fh:
            json.dump(rep, fh, indent=1)
        with open(os.path.join(self.out_dir, 'log.txt'), 'w') as fh:
            fh.write('\n'.join(self.log) + '\n')
        t0 = min([t for t, _ in self.io.tx[:1]] + [t for t, _ in self.io.rx[:1]] or [0])
        with open(os.path.join(self.out_dir, 'midi.jsonl'), 'w') as fh:
            ev = [(t, 'out', m) for t, m in self.io.tx if m != b'\xf8'] + [(t, 'in', m) for t, m in self.io.rx if m != b'\xf8']
            for t, d, m in sorted(ev):
                fh.write(json.dumps({'ms': round((t - t0) * 1000, 2), 'dir': d, 'hex': m.hex()}) + '\n')
        p = sum(r['status'] == 'PASS' for r in self.results)
        f = [r for r in self.results if r['status'] == 'FAIL']
        s = sum(r['status'] == 'SKIP' for r in self.results)
        with open(os.path.join(self.out_dir, 'summary.md'), 'w') as fh:
            fh.write('# MIDI Expression %s on a Digitakt mk1, OS 1.54\n\n' % VERSION)
            fh.write('- Run: %s, %s minutes, label `%s`\n' % (rep['started'], dur, self.label))
            fh.write('- Firmware: %s\n' % self.extra.get('firmware', '?'))
            fh.write('- Ports: in `%s`, out `%s`\n' % (self.io.in_name, self.io.out_name))
            fh.write('- Result: %d passed, %d failed, %d skipped\n' % (p, len(f), s))
            if 'latency_ms' in self.extra:
                fh.write('- Pitch bend round trip: median %(median)s ms, max %(max)s ms (interface included)\n' % self.extra['latency_ms'])
            fh.write('\n| Phase | Check | Result | Note |\n| --- | --- | --- | --- |\n')
            for r in self.results:
                fh.write('| %s | %s | %s | %s |\n' % (r['phase'], r['check'].replace('|', '/'), r['status'], r['detail'].replace('|', '/')))
        print('\n%d passed, %d failed, %d skipped in %s minutes. Results in %s' % (p, len(f), s, dur, self.out_dir))
        for r in f:
            print('  FAIL %s: %s  %s' % (r['phase'], r['check'], r['detail']))


GROUPS = {'core': ['thru', 'routes', 'merge', 'multi', 'latency'], 'play': ['seq', 'flood'],
          'persist': ['persist'], 'ears': ['ears'], 'ui': ['ui']}
GROUPS['all'] = GROUPS['core'] + GROUPS['play'] + GROUPS['persist'] + GROUPS['ui']   # ears and auto: --only


def main():
    ap = argparse.ArgumentParser(description='Hardware qualification for MIDI Expression.')
    ap.add_argument('--setup', action='store_true', help='print the test project to set up once')
    ap.add_argument('--list', action='store_true', help='list the MIDI ports')
    ap.add_argument('--in', dest='inp')
    ap.add_argument('--out')
    ap.add_argument('--label', default='din', help='a name for this run: din, usb ...')
    ap.add_argument('--only', default='all', help='phases or groups, comma separated: ' + ', '.join(GROUPS))
    ap.add_argument('--auto', type=int, default=10, help='AUTO CHANNEL (1-16)')
    ap.add_argument('--tracks', default='11,12,13', help='TRACK CHANNEL of MIDI tracks A,B,C (1-16)')
    ap.add_argument('--outs', default='1,2,3', help='CHAN of MIDI tracks A,B,C (1-16)')
    ap.add_argument('--audio', type=int, default=1, help='TRACK CHANNEL of audio track 1 (1-16)')
    ap.add_argument('--seed', type=int, default=None)
    a = ap.parse_args()
    ta, tb, tc = [int(x) - 1 for x in a.tracks.split(',')]
    oa, ob, oc = [int(x) - 1 for x in a.outs.split(',')]
    cfg = {'auto': a.auto - 1, 'ta': ta, 'tb': tb, 'tc': tc, 'oa': oa, 'ob': ob, 'oc': oc, 'audio': a.audio - 1}
    if a.setup:
        print(SETUP.format(**{k: v + 1 for k, v in cfg.items()}))
        return
    if a.list:
        import rtmidi
        print('inputs: ', rtmidi.MidiIn().get_ports())
        print('outputs:', rtmidi.MidiOut().get_ports())
        return
    phases = []
    for p in a.only.split(','):
        phases += GROUPS.get(p.strip(), [p.strip()])
    seed = a.seed if a.seed is not None else int(time.time())
    random.seed(seed)
    io = Io(a.inp, a.out)
    run = Qual(io, cfg, os.path.join(ROOT, 'results', 'hardware', 'qual-%s-%s' % (a.label, time.strftime('%Y%m%d-%H%M%S'))), a.label)
    run.extra['seed'] = seed
    try:
        if run.intro():
            for p in phases:
                getattr(run, p)()
    except KeyboardInterrupt:
        print('\nstopped')
    finally:
        run.save()
        io.close()


if __name__ == '__main__':
    main()
