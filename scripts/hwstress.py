#!/usr/bin/env python3
"""Stress test for MIDI Expression 0.8 on a real Digitakt (mk1, OS 1.54).

Sends expression, notes and floods to the Digitakt's MIDI IN, records
everything that comes back from its MIDI OUT with timestamps, and checks it
against a model of what 0.8 should send (the same sums the firmware does).
One project setup (README, "Stress test") and a few EXPRESSION-page changes
between phases; the program tells you each one.

    computer MIDI OUT  ->  Digitakt MIDI IN
    Digitakt MIDI OUT  ->  computer MIDI IN

    ~/.venvs/dt/bin/python scripts/hwstress.py --in "MyInterface" --out "MyInterface"
    ~/.venvs/dt/bin/python scripts/hwstress.py --in X --out Y --only thru,routes

Phases: own, thru, release, routes, sens, seq, lfo, record, flood.
Writes results/hardware/stress-<date-time>/: report.json (every check),
log.txt (readable), and midi.jsonl (every message both ways, timestamped)
for analysis. Ctrl+C stops and still saves.
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
from hwtest import Io, ask, yes, wait, hexs, ROOT  # noqa: E402

# The project's own values (README, "Stress test"): set once on MIDI track A.
OWN = {'PB': 8192, 'AT': 0, 'MW': 10, 'BC': 0, 'VAL1': 10, 'VAL2': 0, 'VAL3': 0}
SEL = {'VAL1': 1, 'VAL2': 74, 'VAL3': 2}        # VAL1 shares CC 1 with MW, VAL3 CC 2 with BC
SEQ_VEL = 100                                   # the pattern's trigs


def tdiv(a, b):
    """C integer division (toward zero), as the firmware computes."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def round256(v):
    return (v + 128) >> 8 if v >= 0 else -((-v + 128) >> 8)


def route(src, amount):
    """A route's part, in 1/256 of a MIDI step (expr.c live_parts)."""
    return tdiv(src * amount, 64)


SETTLE = 0.05                   # the Digitakt answers in about 4..40 ms (its MIDI task and the interface)


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def sens(v, s):
    """V.SNS on a velocity (audio.c me_velocity_scaled)."""
    if s >= 100 or v <= 0:
        return v
    return clamp(v + ((127 - v) * (100 - s) + 50) // 100, 1, 127)


class Stress:
    def __init__(self, io, cfg, out_dir):
        self.io, self.cfg, self.out_dir = io, cfg, out_dir
        self.results, self.log = [], []
        self.extra = {}

    # ---- bookkeeping -------------------------------------------------------
    def note(self, text):
        print(text)
        self.log.append(text)

    def check(self, phase, name, ok, detail=''):
        status = 'PASS' if ok is True else 'FAIL' if ok is False else str(ok)
        self.note('  %-5s %s%s' % (status, name, ('  (' + detail + ')') if detail else ''))
        self.results.append({'phase': phase, 'check': name, 'status': status, 'detail': detail})

    @property
    def ch(self):                               # what we send on: the auto channel
        return self.cfg['auto']

    @property
    def o(self):                                # what MIDI track A sends on
        return self.cfg['chan']

    def send(self, *msgs, gap=0.003):
        for m in msgs:
            self.io.send(bytes(m))
            wait(gap)

    def got(self, mark, kinds=(0xB0, 0xD0, 0xE0, 0x90, 0x80)):
        return [(t, m) for t, m in self.io.since(mark) if (m[0] & 0xF0) in kinds and (m[0] & 15) == self.o]

    def last_cc(self, msgs, cc):
        v = [m[2] for _, m in msgs if m[0] & 0xF0 == 0xB0 and m[1] == cc]
        return v[-1] if v else None

    def last(self, msgs, kind):
        v = [m for _, m in msgs if m[0] & 0xF0 == kind]
        if not v:
            return None
        m = v[-1]
        return m[1] | (m[2] << 7) if kind == 0xE0 else m[1]

    def heard(self, mark, kind, cc=None):
        """The value the synth last heard of a kind (or CC number) before mark."""
        g = self.got(0)[:max(0, len(self.got(0)) - len(self.got(mark)))]
        if cc is not None:
            return self.last_cc(g, cc)
        return self.last(g, kind)

    def same_or(self, have, want, mark, kind, cc=None):
        """0.9.1 leaves out a value that did not change, as stock does."""
        return have == want or (have is None and self.heard(mark, kind, cc) == want)

    def valid(self, phase, msgs):
        bad = [hexs(m) for _, m in msgs if any(b > 127 for b in m[1:]) or m[0] < 0x80]
        self.check(phase, 'every message valid (%d)' % len(msgs), not bad, ', '.join(bad[:5]))

    def pb(self, v): return (0xE0 | self.ch, v & 127, v >> 7)
    def at(self, v): return (0xD0 | self.ch, v)
    def cc(self, n, v): return (0xB0 | self.ch, n, v)

    def neutral(self):
        self.send(self.pb(8192), self.at(0), self.cc(1, 0), self.cc(2, 0))
        wait(0.1)

    # ---- phases ----------------------------------------------------------------
    def setup(self):
        c = self.cfg
        self.note('\n== setup')
        print('''
The project as README "Stress test" describes it: MIDI track A with CHAN on,
its knobs and CC SEL numbers set, four trigs on track A, CLOCK SEND on.
MIDI track A selected, its EXPRESSION page at the defaults (THRU ON, no
routes, V.SNS 100 %). Nothing else sending to this MIDI input.''')
        for key, label in (('auto', 'AUTO CHANNEL'), ('chan', "MIDI track A's CHAN")):
            a = ask('%s (1-16) [%d]:' % (label, c[key] + 1)).strip()
            if a.isdigit() and 1 <= int(a) <= 16:
                c[key] = int(a) - 1
        self.note('  auto channel %d, MIDI A sends on %d' % (c['auto'] + 1, c['chan'] + 1))

    def own(self):
        self.note('\n== own: the track sends its own values (the first trig of a pattern sends them all)')
        m = self.io.mark()
        ask('Press PLAY, wait until the first trig has played (a second), press STOP, then Enter.')
        wait(0.2)
        g = self.got(m)
        cc = {}
        for _, x in g:
            if x[0] & 0xF0 == 0xB0:
                cc[x[1]] = x[2]
        self.note('  CCs the track sent: %s' % cc)
        # Stock sends a value only when it changed (all of them at a pattern change):
        # nothing here is fine; 0.9.1 takes the track's own from its parameters.
        for n, want, what in ((1, OWN['MW'], 'MW and VAL1 on CC 1'), (74, OWN['VAL2'], 'VAL2 on CC 74')):
            self.check('own', '%s = %d, if sent' % (what, want), True if n not in cc else cc[n] == want,
                       'got %s' % cc.get(n) if n in cc else 'not sent (unchanged)')
        notes = [x for _, x in g if x[0] & 0xF0 == 0x90 and x[2]]
        self.check('own', 'sequencer note at velocity %d (V.SNS 100 %%)' % SEQ_VEL,
                   bool(notes) and notes[0][2] == SEQ_VEL, 'notes %s' % [hexs(x) for x in notes[:3]])
        self.neutral()

    def thru(self):
        self.note('\n== thru: PB, AT, MW and BC added to the track\'s own; CC 1 and CC 2 merged')
        ask('MIDI track A selected, EXPRESSION page at the defaults (THRU ON, no routes). Enter.')
        m = self.io.mark()
        errs = []
        rng = random.Random(1)
        wheel_moves = 0
        for i in range(200):
            k = rng.randrange(4)
            v = rng.randrange(16384) if k == 0 else rng.randrange(128)
            mk = self.io.mark()
            msg = (self.pb(v), self.at(v), self.cc(1, v), self.cc(2, v))[k]
            self.send(msg, gap=SETTLE)
            g = self.got(mk)
            if k == 0:
                want, have = clamp(OWN['PB'] + v - 8192, 0, 16383), self.last(g, 0xE0)
            elif k == 1:
                want, have = clamp(OWN['AT'] + v, 0, 127), self.last(g, 0xD0)
            elif k == 2:
                want, have = clamp(OWN['MW'] + v, 0, 127), self.last_cc(g, 1)     # VAL1 and MW are both 10
            else:
                want, have = clamp(OWN['BC'] + v, 0, 127), self.last_cc(g, 2)
            wheel_moves += k == 2
            kind, ccn = ((0xE0, None), (0xD0, None), (0xB0, 1), (0xB0, 2))[k]
            if not self.same_or(have, want, mk, kind, ccn):
                errs.append('%s %d -> %s, want %d' % (('PB', 'AT', 'MW', 'BC')[k], v, have, want))
        g = self.got(m)
        self.check('thru', '200 random PB/AT/MW/BC: each out as own + input', not errs, '; '.join(errs[:6]))
        n1 = sum(1 for _, x in g if x[0] & 0xF0 == 0xB0 and x[1] == 1)
        self.check('thru', 'at most one CC 1 per wheel move (MW and VAL1 merged)', 0 < n1 <= wheel_moves, '%d CC 1 for %d moves' % (n1, wheel_moves))
        self.valid('thru', g)
        # the wheel held: nothing below it on CC 1 (a stray stock VAL1 or MW value would be 10)
        self.send(self.cc(1, 90))
        wait(0.05)
        m = self.io.mark()
        wait(0.5)
        self.check('thru', 'wheel held at 90: nothing more on CC 1', self.last_cc(self.got(m), 1) is None, '')

    def release(self):
        self.note('\n== release: leaving the track gives the synth its own values back')
        self.send(self.pb(14000), self.at(90), self.cc(1, 100), self.cc(2, 50))
        wait(0.2)
        m = self.io.mark()
        ask('Press TRK + trig 1 (audio track 1), then Enter.')
        wait(0.2)
        g = self.got(m)
        self.check('release', 'PB back to %d' % OWN['PB'], self.last(g, 0xE0) == OWN['PB'], 'got %s' % self.last(g, 0xE0))
        self.check('release', 'AT back to 0', self.last(g, 0xD0) == OWN['AT'], 'got %s' % self.last(g, 0xD0))
        self.check('release', 'CC 1 back to %d' % OWN['MW'], self.last_cc(g, 1) == OWN['MW'], 'got %s' % self.last_cc(g, 1))
        self.check('release', 'CC 2 back to 0', self.last_cc(g, 2) == 0, 'got %s' % self.last_cc(g, 2))
        m = self.io.mark()
        ask('Press TRK + trig 9 (MIDI track A again), then Enter.')
        wait(0.2)
        self.check('release', 'nothing sent on coming back', not self.got(m), ', '.join(hexs(x) for _, x in self.got(m)))
        self.neutral()

    def routes(self):
        self.note('\n== routes: wheel -> VAL2 (CC 74), pressure -> PB, velocity -> BC')
        print('''
On MIDI track A's EXPRESSION page:
  C  MW  -> CC:CC2 Value #74        D  M.DEP +32   (FUNC + turn D right twice)
  E  VEL -> SRC:BC                  F  V.DEP +64   (FUNC + turn F right four times)
  G  AT  -> SRC:Pitch Bend          H  A.DEP -64   (FUNC + turn H left four times)''')
        ask('Enter when set.')
        m = self.io.mark()
        errs = []
        for w in list(range(0, 128, 7)) + [127, 0]:
            mk = self.io.mark()
            self.send(self.cc(1, w), gap=SETTLE)
            g = self.got(mk)
            want74 = clamp(OWN['VAL2'] + round256(route(w, 32 * 256)), 0, 127)
            want1 = clamp(OWN['MW'] + w, 0, 127)
            if not self.same_or(self.last_cc(g, 74), want74, mk, 0xB0, 74):
                errs.append('wheel %d: CC 74 %s, want %d' % (w, self.last_cc(g, 74), want74))
            if not self.same_or(self.last_cc(g, 1), want1, mk, 0xB0, 1):
                errs.append('wheel %d: CC 1 %s, want %d' % (w, self.last_cc(g, 1), want1))
        self.check('routes', 'wheel: CC 1 own + wheel (THRU), CC 74 + wheel x 32/64', not errs, '; '.join(errs[:6]))
        errs = []
        for p in list(range(0, 128, 9)) + [127, 0]:
            mk = self.io.mark()
            self.send(self.at(p), gap=SETTLE)
            g = self.got(mk)
            want_pb = clamp(OWN['PB'] + tdiv(route(p, -64 * 256) * 16383, 32512), 0, 16383)
            if not self.same_or(self.last(g, 0xE0), want_pb, mk, 0xE0):
                errs.append('pressure %d: PB %s, want %d' % (p, self.last(g, 0xE0), want_pb))
            if not self.same_or(self.last(g, 0xD0), p, mk, 0xD0):
                errs.append('pressure %d: AT %s' % (p, self.last(g, 0xD0)))
        self.check('routes', 'pressure: AT passes, PB pulled down by it (-64 = the whole way)', not errs, '; '.join(errs[:6]))
        errs = []
        order_ok = True
        for v in (1, 20, 64, 100, 127):
            mk = self.io.mark()
            self.send((0x90 | self.ch, 60, v), gap=0.03)
            self.send((0x80 | self.ch, 60, 0), gap=0.02)
            g = self.got(mk)
            seq = [x for _, x in g if (x[0] & 0xF0 == 0xB0 and x[1] == 2) or (x[0] & 0xF0 == 0x90 and x[2])]
            if not seq or seq[0][0] & 0xF0 != 0xB0 or seq[0][2] != clamp(v, 0, 127):
                order_ok = False
                errs.append('velocity %d: %s' % (v, [hexs(x) for x in seq]))
            notes = [x for x in seq if x[0] & 0xF0 == 0x90]
            if not notes or notes[0][2] != v:
                errs.append('velocity %d: note out %s' % (v, [hexs(x) for x in notes]))
        self.check('routes', 'velocity -> BC: CC 2 = velocity, sent before its note; note unchanged', order_ok and not errs, '; '.join(errs[:6]))
        self.valid('routes', self.got(m))
        self.neutral()

    def sens(self):
        self.note('\n== sens: V.SNS 50 % on live notes')
        ask('EXPRESSION page: V.SNS (B) to 50 % (FUNC + turn B left twice). Enter.')
        errs = []
        for v in (1, 20, 64, 100, 127):
            mk = self.io.mark()
            self.send((0x90 | self.ch, 61, v), gap=0.03)
            self.send((0x80 | self.ch, 61, 0), gap=0.02)
            notes = [x for _, x in self.got(mk) if x[0] & 0xF0 == 0x90 and x[2]]
            if not notes or notes[0][2] != sens(v, 50):
                errs.append('%d -> %s, want %d' % (v, [x[2] for x in notes], sens(v, 50)))
            cc2 = self.last_cc(self.got(mk), 2)
            if cc2 != v:
                errs.append('velocity %d: CC 2 (VEL route, the played velocity) %s' % (v, cc2))
        self.check('sens', 'notes out at 50 % (1->64, 64->96 ...); the VEL route takes the played velocity', not errs, '; '.join(errs))

    def seq(self):
        self.note('\n== seq: the sequencer\'s notes through V.SNS 50 %, and the VEL route from them')
        m = self.io.mark()
        ask('Press PLAY, let it play about two bars, press STOP, then Enter.')
        g = self.got(m)
        notes = [x for _, x in g if x[0] & 0xF0 == 0x90 and x[2]]
        vels = sorted(set(x[2] for x in notes))
        self.check('seq', 'trigs at %d play at %d' % (SEQ_VEL, sens(SEQ_VEL, 50)), bool(notes) and vels == [sens(SEQ_VEL, 50)],
                   '%d notes, velocities %s' % (len(notes), vels))
        bad = 0
        for i, (_, x) in enumerate(g):
            if x[0] & 0xF0 == 0x90 and x[2]:
                before = [y for _, y in g[:i] if y[0] & 0xF0 == 0xB0 and y[1] == 2]
                last2 = before[-1][2] if before else self.heard(m, 0xB0, 2)
                if last2 != SEQ_VEL:
                    bad += 1
        self.check('seq', 'each sequencer note plays with CC 2 = %d (VEL route; sent once, unchanged after)' % SEQ_VEL, bool(notes) and not bad, '%d without' % bad)
        self.valid('seq', g)

    def lfo(self):
        self.note('\n== lfo: an LFO on MW, merged with VAL1 on CC 1, and the wheel held')
        print('''
On MIDI track A's LFO page: DEST SRC:Mod Wheel, DEP +40, SPD 64 (fast),
MULT and WAVE as they are. (The EXPRESSION page stays as it is.)''')
        ask('Enter when set. (The LFO runs while the sequencer plays: press PLAY now, then Enter.)')
        m = self.io.mark()
        wait(2.0)
        free = [x[2] for _, x in self.got(m) if x[0] & 0xF0 == 0xB0 and x[1] == 1]
        self.note('  LFO alone (wheel at 0): %d CC 1, range %s..%s' % (len(free), min(free, default=None), max(free, default=None)))
        self.check('lfo', 'the LFO moves CC 1', len(set(free)) > 3, '%d values' % len(set(free)))
        m = self.io.mark()
        self.send(self.cc(1, 64))
        wait(3.0)
        held = [(t, x[2]) for t, x in self.got(m) if x[0] & 0xF0 == 0xB0 and x[1] == 1]
        low = [v for _, v in held[1:] if v < 64]
        self.check('lfo', 'wheel 64 held: every CC 1 at 64 or more (own + 64; a stray stock value would be lower)',
                   bool(held) and not low, '%d CC 1, %d below 64, range %s..%s' % (len(held), len(low), min((v for _, v in held), default=None), max((v for _, v in held), default=None)))
        pairs = [(b[0] - a[0]) * 1000 for a, b in zip(held, held[1:]) if (b[0] - a[0]) < 0.0005 and a[1] != b[1]]
        self.check('lfo', 'never two different CC 1 values at once', not pairs, '%d pairs under 0.5 ms' % len(pairs))
        m = self.io.mark()
        for i in range(150):
            self.send(self.cc(1, (i * 11) % 128), gap=0.01)
        g = self.got(m)
        self.valid('lfo', g)
        self.send(self.cc(1, 0))
        ask('Press STOP. On the LFO page set DEP back to 0. Enter.')
        self.neutral()

    def record(self):
        self.note('\n== record: the sequencer records the velocity as played (V.SNS still 50 %)')
        ask('Hold REC and press PLAY (live recording on MIDI track A), then Enter; the program plays three notes.')
        for v, n in ((20, 64), (60, 65), (120, 66)):
            self.send((0x90 | self.ch, n, v), gap=0.25)
            self.send((0x80 | self.ch, n, 0), gap=0.25)
        ask('Press STOP. EXPRESSION page: V.SNS (B) back to 100 % (FUNC + turn B right twice). Enter.')
        m = self.io.mark()
        ask('Press PLAY, let it play two bars, press STOP, then Enter.')
        notes = {x[1]: x[2] for _, x in self.got(m) if x[0] & 0xF0 == 0x90 and x[2] and x[1] in (64, 65, 66)}
        self.check('record', 'played back at the velocities played: 20, 60, 120 (scaled would be 74, 94, 124)',
                   notes == {64: 20, 65: 60, 66: 120}, 'got %s' % notes)
        print('(You can delete the recorded notes now: they are on MIDI track A.)')

    def flood(self, seconds=12):
        self.note('\n== flood: clock and sequencer timing with expression flooding in')
        ask('Turn up the tempo (150 BPM or more), press PLAY, then Enter.')

        def measure(flood):
            m = self.io.mark()
            t_end = time.perf_counter() + seconds
            i = 0
            while time.perf_counter() < t_end:
                if flood:
                    self.io.send(self.pb((i * 97) % 16384))
                    self.io.send(self.at((i * 13) % 128))
                    self.io.send(self.cc(1, (i * 7) % 128))
                    self.io.send(self.cc(2, (i * 5) % 128))
                    i += 1
                    wait(0.004)                 # about the DIN line's whole rate
                else:
                    wait(0.05)
            got = self.io.since(m, clock=True)
            ticks = [t for t, x in got if x == b'\xf8']
            iv = [(b - a) * 1000 for a, b in zip(ticks, ticks[1:])]
            notes = [t for t, x in got if x[0] & 0xF0 == 0x90 and len(x) > 2 and x[2]]
            st = {'ticks': len(ticks), 'notes': len(notes), 'in': i * 4,
                  'out': sum(1 for _, x in got if x[0] & 0xF0 in (0xB0, 0xD0, 0xE0))}
            if len(iv) > 10:
                mean = statistics.mean(iv)
                st.update(mean_ms=round(mean, 3), stdev_ms=round(statistics.pstdev(iv), 3),
                          worst_ms=round(max(abs(x - mean) for x in iv), 3))
            nv = [(b - a) * 1000 for a, b in zip(notes, notes[1:])]
            if len(nv) > 4:
                st['note_stdev_ms'] = round(statistics.pstdev(nv), 3)
            return st, [x for _, x in got if x != b'\xf8']

        base, _ = measure(False)
        load, msgs = measure(True)
        self.neutral()
        self.note('  baseline: %s' % json.dumps(base))
        self.note('  flooded:  %s' % json.dumps(load))
        self.extra['flood'] = {'baseline': base, 'flooded': load}
        if base.get('stdev_ms') is not None and load.get('stdev_ms') is not None:
            self.check('flood', 'clock jitter increase under 1 ms', load['stdev_ms'] - base['stdev_ms'] < 1.0,
                       'stdev %.3f -> %.3f ms, worst %.3f -> %.3f ms' % (base['stdev_ms'], load['stdev_ms'], base['worst_ms'], load['worst_ms']))
            self.check('flood', 'no sequencer notes lost', load['notes'] >= base['notes'] - 2, '%d -> %d notes' % (base['notes'], load['notes']))
        else:
            self.check('flood', 'clock received', False, 'no clock: CLOCK SEND on and PLAY pressed?')
        self.valid('flood', [(0, x) for x in msgs if len(x) > 1])
        self.check('flood', 'the panel stayed responsive', yes('Did the screen and keys stay responsive?'))
        ask('Press STOP, then Enter.')
        wait(0.3)
        held = {}
        for _, x in self.io.since(0):
            if x[0] & 0xF0 in (0x90, 0x80) and (x[0] & 15) == self.o and len(x) > 2:
                on = x[0] & 0xF0 == 0x90 and x[2] > 0
                held[x[1]] = held.get(x[1], 0) + (1 if on else -1)
        stuck = [n for n, c in held.items() if c > 0]
        self.check('flood', 'every note on got its note off', not stuck, 'stuck notes %s' % stuck)

    # ---- report ------------------------------------------------------------------
    def save(self):
        os.makedirs(self.out_dir, exist_ok=True)
        rep = {'when': time.strftime('%Y-%m-%d %H:%M:%S'), 'version': '0.10.0-experimental',
               'ports': {'in': self.io.in_name, 'out': self.io.out_name},
               'config': {k: v + 1 for k, v in self.cfg.items()}, 'own': OWN, 'sel': SEL,
               'results': self.results, **self.extra}
        with open(os.path.join(self.out_dir, 'report.json'), 'w') as fh:
            json.dump(rep, fh, indent=1)
        with open(os.path.join(self.out_dir, 'log.txt'), 'w') as fh:
            fh.write('\n'.join(self.log) + '\n')
        t0 = min([t for t, _ in self.io.tx[:1]] + [t for t, _ in self.io.rx[:1]] or [0])
        with open(os.path.join(self.out_dir, 'midi.jsonl'), 'w') as fh:
            ev = [(t, 'out', m) for t, m in self.io.tx] + [(t, 'in', m) for t, m in self.io.rx if m != b'\xf8']
            for t, d, m in sorted(ev):
                fh.write(json.dumps({'ms': round((t - t0) * 1000, 2), 'dir': d, 'hex': m.hex()}) + '\n')
        p = sum(r['status'] == 'PASS' for r in self.results)
        f = sum(r['status'] == 'FAIL' for r in self.results)
        print('\n%d passed, %d failed. Results in %s' % (p, f, self.out_dir))


def main():
    ap = argparse.ArgumentParser(description='Stress test for MIDI Expression 0.8.')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--in', dest='inp')
    ap.add_argument('--out')
    ap.add_argument('--only', help='phases: own,thru,release,routes,sens,seq,lfo,record,flood')
    ap.add_argument('--auto', type=int, default=10)
    ap.add_argument('--chan', type=int, default=1)
    a = ap.parse_args()
    if a.list:
        import rtmidi
        print('inputs: ', rtmidi.MidiIn().get_ports())
        print('outputs:', rtmidi.MidiOut().get_ports())
        return
    order = ['own', 'thru', 'release', 'routes', 'sens', 'seq', 'lfo', 'record', 'flood']
    phases = a.only.split(',') if a.only else order
    io = Io(a.inp, a.out)
    run = Stress(io, {'auto': a.auto - 1, 'chan': a.chan - 1},
                 os.path.join(ROOT, 'results', 'hardware', 'stress-' + time.strftime('%Y%m%d-%H%M%S')))
    try:
        run.setup()
        for p in phases:
            getattr(run, p)()
    except KeyboardInterrupt:
        print('\nstopped')
    finally:
        run.save()
        io.close()


if __name__ == '__main__':
    main()
