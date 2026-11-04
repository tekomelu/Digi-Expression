#!/usr/bin/env python3
"""Hardware tests for MIDI Expression on a real Digitakt (mk1, OS 1.54).

Runs on your Mac (or any computer) with a MIDI interface:

    computer MIDI OUT  ->  Digitakt MIDI IN
    Digitakt MIDI OUT  ->  computer MIDI IN

It sends test messages, records everything that comes back with
timestamps, asks you to press keys on the Digitakt where a test needs it,
and writes results/hardware/<date-time>/report.json and log.txt. Send that
folder back for analysis.

    python3 -m pip install --user python-rtmidi
    python3 scripts/hwtest.py --list                      # show the MIDI ports
    python3 scripts/hwtest.py --in "Interface 2" --out "Interface 2"
    python3 scripts/hwtest.py --in X --out Y --only passthru,latency

Port names match by any part of the name (case-insensitive). Without
--in/--out it asks you to pick from a list.

Tests: setup, passthru, unticked, restore, latency, audio, clock.
Nothing here writes to the Digitakt except plain MIDI messages; it never
sends SysEx.
"""
import argparse
import json
import os
import statistics
import sys
import threading
import time

try:
    import rtmidi
except ImportError:
    sys.exit('python-rtmidi is missing: python3 -m pip install --user python-rtmidi')

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


# ---- MIDI I/O ----------------------------------------------------------------
class Io:
    def __init__(self, in_name, out_name):
        self.mi = rtmidi.MidiIn()
        self.mo = rtmidi.MidiOut()
        self.mi.ignore_types(sysex=True, timing=False, active_sense=True)
        self.in_name = self._open(self.mi, in_name, 'input')
        self.out_name = self._open(self.mo, out_name, 'output')
        self.lock = threading.Lock()
        self.rx = []                         # (t, bytes)
        self.tx = []                         # (t, bytes)
        self.mi.set_callback(self._on_rx)

    @staticmethod
    def pick(port_obj, want, kind):
        names = port_obj.get_ports()
        if not names:
            sys.exit('no MIDI %s ports found' % kind)
        if want:
            hits = [i for i, n in enumerate(names) if want.lower() in n.lower()]
            if len(hits) == 1:
                return hits[0], names[hits[0]]
            if not hits:
                sys.exit('no %s port matches %r; have: %s' % (kind, want, ', '.join(names)))
            sys.exit('%r matches several %s ports: %s' % (want, kind, ', '.join(names[i] for i in hits)))
        print('\nMIDI %s ports:' % kind)
        for i, n in enumerate(names):
            print('  %d  %s' % (i, n))
        while True:
            a = input('Pick the %s port number (the one %s the Digitakt): ' % (
                kind, 'receiving from' if kind == 'input' else 'sending to'))
            if a.strip().isdigit() and int(a) < len(names):
                return int(a), names[int(a)]

    def _open(self, port_obj, want, kind):
        i, name = self.pick(port_obj, want, kind)
        port_obj.open_port(i)
        return name

    def _on_rx(self, event, data=None):
        msg, _delta = event
        with self.lock:
            self.rx.append((time.perf_counter(), bytes(msg)))

    def send(self, data):
        t = time.perf_counter()
        self.mo.send_message(list(data))
        with self.lock:
            self.tx.append((t, bytes(data)))
        return t

    def mark(self):
        with self.lock:
            return len(self.rx)

    def since(self, mark, clock=False):
        with self.lock:
            got = self.rx[mark:]
        return [(t, m) for t, m in got if clock or m[0] not in (0xF8, 0xFE)]

    def close(self):
        self.mi.close_port()
        self.mo.close_port()


def hexs(m):
    return ' '.join('%02x' % b for b in m)


def ask(text):
    return input('\n>>> ' + text + ' ')


def yes(text):
    while True:
        a = ask(text + ' [y/n]').strip().lower()
        if a in ('y', 'n'):
            return a == 'y'


def wait(s):
    time.sleep(s)


# ---- tests -------------------------------------------------------------------
class Run:
    def __init__(self, io, cfg, out_dir):
        self.io, self.cfg, self.out_dir = io, cfg, out_dir
        self.results = []
        self.log = []

    def note(self, text):
        print(text)
        self.log.append(text)

    def result(self, test, name, ok, detail=''):
        status = 'PASS' if ok is True else 'FAIL' if ok is False else str(ok)
        self.note('  %-5s %s%s' % (status, name, ('  (' + detail + ')') if detail else ''))
        self.results.append({'test': test, 'check': name, 'status': status, 'detail': detail})

    def expect(self, test, name, send_msgs, want, window=0.25):
        """Send messages, wait, and check that each wanted message came back
        (in any order) and nothing else of the watched types."""
        m = self.io.mark()
        t0 = None
        for msg in send_msgs:
            t = self.io.send(msg)
            t0 = t0 or t
            wait(0.004)
        wait(window)
        got = self.io.since(m)
        for t, msg in got:
            self.log.append('    rx +%6.1f ms  %s' % ((t - t0) * 1000, hexs(msg)))
        remaining = [bytes(w) for w in want]
        lat = []
        extra = []
        for t, msg in got:
            if msg in remaining:
                remaining.remove(msg)
                lat.append((t - t0) * 1000)
            elif msg[0] & 0xF0 in (0xE0, 0xD0, 0xB0, 0x90, 0x80):
                extra.append(hexs(msg))
        ok = not remaining and not extra
        detail = []
        if remaining:
            detail.append('missing ' + ', '.join(hexs(r) for r in remaining))
        if extra:
            detail.append('unexpected ' + ', '.join(extra))
        if lat:
            detail.append('latency %.1f-%.1f ms' % (min(lat), max(lat)))
        self.result(name.split(':')[0] if ':' in name else test, name, ok, '; '.join(detail))
        return ok

    # messages
    def pb(self, ch, v): return bytes([0xE0 | ch, v & 0x7F, v >> 7])
    def at(self, ch, v): return bytes([0xD0 | ch, v])
    def mw(self, ch, v): return bytes([0xB0 | ch, 1, v])
    def non(self, ch, n, v=100): return bytes([0x90 | ch, n, v])
    def noff(self, ch, n): return bytes([0x80 | ch, n, 0])

    def setup(self):
        c = self.cfg
        self.note('\n== setup')
        print('''
On the Digitakt (with the MIDI Expression build):
  SETTINGS > MIDI CONFIG > PORT CONFIG: INPUT FROM includes MIDI, OUTPUT TO
      includes MIDI, RECEIVE NOTES and RECEIVE CC/NRPN ticked.
  SETTINGS > MIDI CONFIG > CHANNELS: note AUTO CHANNEL and TRACK A CHANNEL.
  Select MIDI track A (TRK + trig 9); on its SRC page press FUNC + knob A so
      CHAN is on, and note its channel.
  On each track's EXPRESSION page (TRIG, then tap TRIG) leave the defaults
      (they come back at every power-on): BEND 2, MW, VEL and AT none (---),
      depths 0, V.SNS 100 %; MIDI A: PB, AT, MW ON, MW> CC 1, AT> AT.
  Nothing else should send to this MIDI input during the tests.''')
        for key, label in (('auto', 'AUTO CHANNEL'), ('track_a', 'TRACK A CHANNEL'), ('chan', "MIDI track A's CHAN")):
            a = ask('%s (1-16) [%d]:' % (label, c[key] + 1)).strip()
            if a.isdigit() and 1 <= int(a) <= 16:
                c[key] = int(a) - 1
        ask('Press Enter when MIDI track A is selected.')
        self.note('  auto ch %d, track A ch %d, out ch %d' % (c['auto'] + 1, c['track_a'] + 1, c['chan'] + 1))

    def passthru(self):
        c = self.cfg
        self.note('\n== passthru (MIDI track A selected)')
        for label, ch in (('auto channel', c['auto']), ("track A's channel", c['track_a'])):
            o = c['chan']
            self.expect('passthru', 'note via %s' % label, [self.non(ch, 60)], [self.non(o, 60)])
            self.expect('passthru', 'note off via %s' % label, [self.noff(ch, 60)], [bytes([0x90 | o, 60, 0])])
            self.expect('passthru', 'pitch bend via %s' % label,
                        [self.pb(ch, 12288), self.pb(ch, 16383), self.pb(ch, 0), self.pb(ch, 8192)],
                        [self.pb(o, 12288), self.pb(o, 16383), self.pb(o, 0), self.pb(o, 8192)], 0.4)
            self.expect('passthru', 'aftertouch via %s' % label, [self.at(ch, 80), self.at(ch, 0)],
                        [self.at(o, 80), self.at(o, 0)])
            self.expect('passthru', 'mod wheel via %s' % label, [self.mw(ch, 64), self.mw(ch, 0)],
                        [self.mw(o, 64), self.mw(o, 0)])

    def unticked(self):
        c = self.cfg
        self.note('\n== switched off (stock behaviour)')
        ask('On MIDI track A\'s EXPRESSION page turn PB, AT and MW (knobs A-C) OFF, keep MIDI track A selected, then press Enter.')
        ch, o = c['auto'], c['chan']
        self.expect('unticked', 'notes still pass', [self.non(ch, 62), self.noff(ch, 62)],
                    [self.non(o, 62), bytes([0x90 | o, 62, 0])])
        self.expect('unticked', 'PB, AT, MW dropped as stock',
                    [self.pb(ch, 12000), self.at(ch, 50), self.mw(ch, 50)], [])
        ask('Turn PB, AT and MW back ON, then press Enter.')

    def restore(self):
        c = self.cfg
        self.note('\n== restore (leaving a track gives the synth its own values back)')
        ch, o = c['auto'], c['chan']
        ask('Select MIDI track A (TRK + trig 9) and leave its PB, AT and MW knobs at their defaults. Press Enter.')
        for msg in (self.pb(ch, 14000), self.at(ch, 90), self.mw(ch, 100)):
            self.io.send(msg)
            wait(0.01)
        wait(0.2)
        m = self.io.mark()
        ask('Now press TRK + trig 1 (audio track 1), then press Enter here straight away.')
        t_enter = time.perf_counter()
        wait(0.3)
        got = self.io.since(m)
        for t, msg in got:
            self.log.append('    rx %+7.1f ms from Enter  %s' % ((t - t_enter) * 1000, hexs(msg)))
        kinds = {msg[0] & 0xF0: msg for t, msg in got if msg[0] & 0x0F == o}
        pb_ok = 0xE0 in kinds
        at_ok = 0xD0 in kinds
        mw_ok = any(msg[0] & 0xF0 == 0xB0 and msg[1] == 1 for t, msg in got)
        self.result('restore', 'track values back on leaving A', pb_ok and at_ok and mw_ok,
                    'got: ' + ', '.join(hexs(m_) for _, m_ in got) if got else 'nothing arrived')
        m = self.io.mark()
        ask('Press TRK + trig 9 (back to MIDI track A), then press Enter.')
        wait(0.3)
        got = [x for x in self.io.since(m) if x[1][0] & 0xF0 in (0xE0, 0xD0, 0xB0)]
        self.result('restore', 'nothing sent when coming back', not got,
                    ', '.join(hexs(m_) for _, m_ in got))
        self.expect('restore', 'A follows the next wheel move', [self.mw(ch, 20)], [self.mw(o, 20)])
        self.io.send(self.mw(ch, 0))
        wait(0.05)

    def latency(self, n=200):
        c = self.cfg
        self.note('\n== latency (%d pitch bends, 10 ms apart, MIDI track A selected)' % n)
        ask('Make sure MIDI track A is selected and the sequencer is stopped. Press Enter.')
        ch, o = c['auto'], c['chan']
        m = self.io.mark()
        sent = []
        for i in range(n):
            v = 1000 + i * 50
            sent.append((self.io.send(self.pb(ch, v)), v))
            wait(0.010)
        wait(0.3)
        got = [(t, msg) for t, msg in self.io.since(m) if msg[0] == 0xE0 | o]
        by_value = {}
        for t, msg in got:
            by_value.setdefault(msg[1] | msg[2] << 7, t)
        lat = [(by_value[v] - t) * 1000 for t, v in sent if v in by_value]
        self.io.send(self.pb(ch, 8192))
        if lat:
            lat.sort()
            detail = '%d of %d back; min %.1f, median %.1f, 95%% %.1f, max %.1f ms' % (
                len(lat), n, lat[0], statistics.median(lat), lat[int(len(lat) * 0.95) - 1], lat[-1])
        else:
            detail = 'none came back'
        self.result('latency', 'pitch bend round trip', len(lat) >= n * 0.98, detail)
        self.latency_ms = lat

    def audio(self):
        c = self.cfg
        self.note('\n== audio tracks (listening checks)')
        print('''
Track 1 needs a sample with a clear pitch and a long sound (a sustained
synth or a long LOOP). The program plays it with a note on track 1's own
channel (TRACK 1 CHANNEL in MIDI CONFIG > CHANNELS).''')
        a = ask('TRACK 1 CHANNEL (1-16) [1]:').strip()
        t1 = int(a) - 1 if a.isdigit() and 1 <= int(a) <= 16 else 0
        ask('Select audio track 1 (TRK + trig 1). Its EXPRESSION page (TRIG, tap TRIG): BEND 2. Press Enter.')
        ch = c['auto']
        m = self.io.mark()
        self.io.send(self.non(t1, 60))
        wait(0.6)
        for v in list(range(8192, 16384, 256)) + [16383] * 10 + list(range(16383, 8192, -256)) + [8192]:
            self.io.send(self.pb(ch, v))
            wait(0.02)
        wait(0.5)
        self.io.send(self.noff(t1, 60))
        got = [x for x in self.io.since(m) if x[1][0] & 0xF0 == 0xE0]
        self.result('audio', 'no pitch bend sent to MIDI OUT for an audio track', not got,
                    '%d pitch bends came out' % len(got) if got else '')
        self.result('audio', 'bend up 2 semitones and back (heard)',
                    yes('Did the note bend up by two semitones and come back?'))
        ask('On track 1\'s EXPRESSION page turn knob A to BEND 12, then press Enter.')
        self.io.send(self.non(t1, 60))
        wait(0.4)
        self.io.send(self.pb(ch, 16383))
        wait(0.8)
        self.io.send(self.pb(ch, 0))
        wait(0.8)
        self.io.send(self.pb(ch, 8192))
        wait(0.3)
        self.io.send(self.noff(t1, 60))
        self.result('audio', 'BEND 12: an octave up, an octave down (heard)',
                    yes('Did it jump an octave up, then an octave down, then back?'))
        ask('EXPRESSION page: MW (knob C) FLTR:Frequency, M.DEP (D) +32 (FUNC + turn D twice); on the FLTR page close the filter about halfway. Press Enter.')
        self.io.send(self.non(t1, 60))
        for v in list(range(0, 128, 4)) + list(range(127, -1, -4)):
            self.io.send(self.mw(ch, v))
            wait(0.03)
        self.io.send(self.mw(ch, 0))
        wait(0.2)
        self.io.send(self.noff(t1, 60))
        self.result('audio', 'mod wheel opens the filter and closes it again (heard)',
                    yes('Did the filter open and close smoothly, back where it was?'))
        ask('EXPRESSION page: AT (knob G) LFO1:Depth, A.DEP (H) +32 (FUNC + turn H twice); give track 1 LFO1 a slow wave on pitch or filter with DEP 0. Press Enter.')
        self.io.send(self.non(t1, 60))
        for v in list(range(0, 128, 4)) + [127] * 20 + list(range(127, -1, -4)):
            self.io.send(self.at(ch, v))
            wait(0.03)
        self.io.send(self.at(ch, 0))
        wait(0.2)
        self.io.send(self.noff(t1, 60))
        self.result('audio', 'pressure brings in the LFO, release takes it away (heard)',
                    yes('Did the LFO wobble appear with pressure and stop when it went back to 0?'))
        def three_notes():
            for v in (20, 60, 127):
                self.io.send(bytes([0x90 | t1, 60, v]))
                wait(0.5)
                self.io.send(self.noff(t1, 60))
                wait(0.3)
        ask('Back on the EXPRESSION page: V.SNS (knob B) 100 %. Press Enter, then listen: three notes, soft to hard.')
        three_notes()
        loud = yes('Did they get louder, soft to hard?')
        ask('Turn V.SNS to 0 %. Press Enter, then listen to the same three notes.')
        three_notes()
        self.result('audio', 'V.SNS: 100 % follows velocity, 0 % plays every note full (heard)',
                    loud and yes('Were they all equally loud now?'))
        ask('V.SNS back to 100 % (FUNC + turn B right). VEL (knob E) FLTR:Frequency, V.DEP (F) +32 (FUNC + turn F twice); the filter closed about halfway. Press Enter.')
        three_notes()
        self.result('audio', 'VEL to the filter: harder notes brighter (heard)',
                    yes('Did the harder notes sound brighter?'))
        ask('EXPRESSION page: set MW, VEL and AT (knobs C, E, G) back to none (FUNC + turn left until ---), BEND (A) back to 2. Press Enter.')

    def clock(self, seconds=15):
        c = self.cfg
        self.note('\n== clock and sequencer timing under heavy expression')
        print('''
Set up the worst case you can:
  SETTINGS > MIDI CONFIG > SYNC: CLOCK SEND ticked.
  A pattern with trigs on several MIDI tracks (chords if you like), all with
  CHAN on, ideally on every step, at 150 BPM or more.
  MIDI track A selected, its PB, AT and MW ON.''')
        ask('Press PLAY on the Digitakt, then Enter here.')
        ch = c['auto']

        def measure(flood):
            m = self.io.mark()
            t_end = time.perf_counter() + seconds
            i = 0
            while time.perf_counter() < t_end:
                if flood:
                    v = (i * 97) % 16384
                    self.io.send(self.pb(ch, v))
                    self.io.send(self.at(ch, (i * 13) % 128))
                    i += 1
                    wait(0.002)
                else:
                    wait(0.05)
            got = self.io.since(m, clock=True)
            ticks = [t for t, msg in got if msg == b'\xf8']
            notes = [t for t, msg in got if msg[0] & 0xF0 == 0x90 and len(msg) > 2 and msg[2]]
            exprs = [t for t, msg in got if msg[0] & 0xF0 in (0xE0, 0xD0)]
            iv = [(b - a) * 1000 for a, b in zip(ticks, ticks[1:])]
            stats = {}
            if len(iv) > 10:
                mean = statistics.mean(iv)
                stats = {'ticks': len(ticks), 'mean_ms': round(mean, 3),
                         'stdev_ms': round(statistics.pstdev(iv), 3),
                         'worst_dev_ms': round(max(abs(x - mean) for x in iv), 3),
                         'bpm': round(60000 / (mean * 24), 2)}
            stats.update({'notes': len(notes), 'expression_out': len(exprs), 'expression_in': i * 2})
            return stats

        base = measure(False)
        self.note('  baseline:        %s' % json.dumps(base))
        loaded = measure(True)
        self.io.send(self.pb(ch, 8192))
        self.io.send(self.at(ch, 0))
        self.note('  with expression: %s' % json.dumps(loaded))
        self.clock_stats = {'baseline': base, 'flood': loaded}
        if base.get('ticks') and loaded.get('ticks'):
            worse = loaded['stdev_ms'] - base['stdev_ms']
            self.result('clock', 'clock jitter with expression vs without',
                        'NOTE', 'stdev %.3f -> %.3f ms, worst %.3f -> %.3f ms, notes %d -> %d' % (
                            base['stdev_ms'], loaded['stdev_ms'], base['worst_dev_ms'],
                            loaded['worst_dev_ms'], base['notes'], loaded['notes']))
            self.result('clock', 'jitter increase under 1 ms', worse < 1.0, '%+.3f ms' % worse)
        else:
            self.result('clock', 'clock received', False, 'no MIDI clock arrived: CLOCK SEND on, PLAY pressed?')
        self.result('clock', 'panel stayed responsive', yes('Did the Digitakt stay responsive (screen, keys) during the flood?'))
        ask('Press STOP on the Digitakt, then Enter.')
        wait(0.3)
        self.result('clock', 'no stuck notes after STOP', yes('Is everything silent (no hanging synth notes)?'))

    def save(self):
        os.makedirs(self.out_dir, exist_ok=True)
        report = {
            'when': time.strftime('%Y-%m-%d %H:%M:%S'),
            'ports': {'in': self.io.in_name, 'out': self.io.out_name},
            'config': {k: v + 1 for k, v in self.cfg.items()},
            'results': self.results,
            'latency_ms': [round(x, 2) for x in getattr(self, 'latency_ms', [])],
            'clock': getattr(self, 'clock_stats', None),
        }
        with open(os.path.join(self.out_dir, 'report.json'), 'w') as fh:
            json.dump(report, fh, indent=1)
        with open(os.path.join(self.out_dir, 'log.txt'), 'w') as fh:
            fh.write('\n'.join(self.log) + '\n')
            fh.write('\n# every message received\n')
            t0 = self.io.rx[0][0] if self.io.rx else 0
            for t, m in self.io.rx:
                if m != b'\xf8':
                    fh.write('%10.1f  %s\n' % ((t - t0) * 1000, hexs(m)))
        passed = sum(r['status'] == 'PASS' for r in self.results)
        failed = sum(r['status'] == 'FAIL' for r in self.results)
        print('\n%d passed, %d failed. Results in %s' % (passed, failed, self.out_dir))


def main():
    ap = argparse.ArgumentParser(description='Hardware tests for MIDI Expression.')
    ap.add_argument('--list', action='store_true', help='list MIDI ports and exit')
    ap.add_argument('--in', dest='inp', help='input port (part of its name): what the Digitakt sends to')
    ap.add_argument('--out', help='output port (part of its name): what goes to the Digitakt MIDI IN')
    ap.add_argument('--only', help='comma-separated tests: setup,passthru,unticked,restore,latency,audio,clock')
    ap.add_argument('--auto', type=int, default=14, help='AUTO CHANNEL, 1-16 (asked again in setup)')
    ap.add_argument('--track-a', type=int, default=9, help='TRACK A CHANNEL, 1-16')
    ap.add_argument('--chan', type=int, default=1, help="MIDI track A's CHAN, 1-16")
    args = ap.parse_args()
    if args.list:
        print('inputs: ', rtmidi.MidiIn().get_ports())
        print('outputs:', rtmidi.MidiOut().get_ports())
        return
    order = ['setup', 'passthru', 'unticked', 'restore', 'latency', 'audio', 'clock']
    tests = args.only.split(',') if args.only else order
    if 'setup' not in tests:
        tests = ['setup'] + tests
    io = Io(args.inp, args.out)
    print('in: %s\nout: %s' % (io.in_name, io.out_name))
    cfg = {'auto': args.auto - 1, 'track_a': args.track_a - 1, 'chan': args.chan - 1}
    out_dir = os.path.join(ROOT, 'results', 'hardware', time.strftime('%Y%m%d-%H%M%S'))
    run = Run(io, cfg, out_dir)
    try:
        for name in order:
            if name in tests:
                getattr(run, name)()
    except KeyboardInterrupt:
        run.note('\n(stopped by the user)')
    finally:
        for ch in range(16):                 # leave no bend or pressure behind
            io.send(bytes([0xE0 | ch, 0, 64]))
            io.send(bytes([0xD0 | ch, 0]))
        run.save()
        io.close()


if __name__ == '__main__':
    main()
