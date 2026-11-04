#!/usr/bin/env python3
"""Record an audio track's note in the emulator and measure pitch differences.

Emulator only (digiemu, audio on). Each case starts a fresh session from the
same snapshot, sends its MIDI (hex messages, ';'-separated), records the
audio, and saves <out>/<case>.wav. `compare` then measures how many
semitones each recording sits above or below the reference, by sliding the
spectra against each other on a log-frequency axis (0.01-semitone steps).

    set -a; . toolchain/emu-mx030.env; set +a
    P=digiemu/.venv/bin/python
    $P scripts/pitch-test.py record --snapshot S --out captures/pitch c4     "90 3c 64"
    $P scripts/pitch-test.py record --snapshot S --out captures/pitch d4     "90 3e 64"
    $P scripts/pitch-test.py record --snapshot S --out captures/pitch c4-up2 "e0 7f 7f; 90 3c 64"
    $P scripts/pitch-test.py compare captures/pitch c4 d4 c4-up2
"""
import argparse
import os
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def record(args):
    import dtsession
    from unicorn.m68k_const import UC_M68K_REG_PC
    from emu.session import Session
    syx = os.environ['DT2_SYX']
    snapshot = os.path.abspath(args.snapshot)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    os.chdir(os.path.dirname(syx))
    io = dtsession.Midi()
    s = Session(snapshot, syx, audio=True, on_machine=io.on_machine)
    try:
        s.run_ms(100)
        for msg in [m for m in args.midi.split(';') if m.strip()]:
            io.midi_in.put(bytes(int(t, 16) for t in msg.split()))
            for _ in range(100):
                if io.midi_in.deliver(s.m):
                    s.pc = s.m.uc.reg_read(UC_M68K_REG_PC)
                if not io.midi_in._pending:
                    break
                s.run_ms(1)
            s.run_ms(5)
        start = len(s.pcm)
        s.run_ms(args.ms)
        pcm = bytes(s.pcm[start:])
        rate = s.audio_cfg['rate']
        path = os.path.join(out, args.case + '.wav')
        with wave.open(path, 'wb') as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(pcm)
        print('%s: %.2f s at %d Hz -> %s%s' % (args.case, len(pcm) / 4 / rate, rate, path,
                                                '  HALTED ' + s.halted if s.halted else ''))
    finally:
        s.close()


def spectrum(path):
    import numpy as np
    with wave.open(path) as w:
        rate = w.getframerate()
        data = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2').astype(float)
    mono = data.reshape(-1, 2).mean(axis=1)
    n = 1 << int(np.floor(np.log2(len(mono))))
    seg = mono[len(mono) - n:] * np.hanning(n)        # the later, steadier part
    mag = np.abs(np.fft.rfft(seg))
    freqs = np.fft.rfftfreq(n, 1 / rate)
    # resample onto a log axis: 0.01 semitone per bin, 40 Hz .. 10 kHz
    lo, hi = np.log2(40), np.log2(min(10000, rate / 2))
    grid = np.arange(lo, hi, 1 / 1200)
    logmag = np.interp(2 ** grid, freqs, mag)
    rms = float(np.sqrt(np.mean(mono ** 2)))
    return np.log1p(logmag), rms


def compare(args):
    import numpy as np
    ref, ref_rms = spectrum(os.path.join(args.dir, args.ref + '.wav'))
    print('%-10s rms %8.1f  (reference)' % (args.ref, ref_rms))
    for case in args.cases:
        sp, rms = spectrum(os.path.join(args.dir, case + '.wav'))
        best, best_shift = -1e18, 0
        a = ref - ref.mean()
        b = sp - sp.mean()
        for shift in range(-2500, 2501):              # +-25 semitones
            if shift >= 0:
                c = float(np.dot(a[:len(a) - shift], b[shift:]))
            else:
                c = float(np.dot(a[-shift:], b[:len(b) + shift]))
            if c > best:
                best, best_shift = c, shift
        print('%-10s rms %8.1f  %+6.2f semitones from %s' % (case, rms, best_shift / 100, args.ref))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('record')
    r.add_argument('--snapshot', required=True)
    r.add_argument('--out', required=True)
    r.add_argument('--ms', type=int, default=500)
    r.add_argument('case')
    r.add_argument('midi')
    c = sub.add_parser('compare')
    c.add_argument('dir')
    c.add_argument('ref')
    c.add_argument('cases', nargs='+')
    args = ap.parse_args()
    record(args) if args.cmd == 'record' else compare(args)


if __name__ == '__main__':
    main()
