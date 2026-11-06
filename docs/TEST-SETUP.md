# The test project: EXPR TEST

One project, set up once, that `scripts/hwqual.py` tests automatically: all eight MIDI tracks, each set up to test a different part of the mod, and all eight audio tracks for the by-eye and by-ear checks. Save it as **EXPR TEST** and keep it: every later test run uses it.

Start from an empty project (SETTINGS > PROJECT > LOAD PROJECT > CREATE NEW).

## Connections

Connect **both** at once, so every run tests DIN and USB together:

- Computer's MIDI interface OUT to the Digitakt's MIDI IN, and the Digitakt's MIDI OUT to the interface's IN.
- The Digitakt's USB port to the computer.

The program sends on one, listens on both, then swaps. It checks that what goes out of DIN and out of USB is the same: DIN in to USB out, USB in to DIN out, and each to itself.

## SETTINGS > MIDI CONFIG

| Page | Setting | Value |
| --- | --- | --- |
| SYNC | CLOCK RECEIVE | ON |
| SYNC | CLOCK SEND | as you like (the program ignores clock from the Digitakt) |
| SYNC | TRANSPORT RECEIVE | ON |
| SYNC | TRANSPORT SEND | as you like |
| PORT CONFIG | INPUT FROM | MIDI+USB |
| PORT CONFIG | OUTPUT TO | MIDI+USB |
| PORT CONFIG | RECEIVE NOTES | ON |
| PORT CONFIG | RECEIVE CC/NRPN | ON |
| CHANNELS | TRACK 1 … 8 CHANNEL | 1 … 8 |
| CHANNELS | TRACK A … H CHANNEL | 9 … 16 |
| CHANNELS | FX CHANNEL | OFF |
| CHANNELS | AUTO CHANNEL | OFF (if your OS has no OFF, see "The auto channel" below) |

## MIDI tracks A to H

Select a track with TRK + trig 9 … 16. For each: on the **SRC** page set **CHAN** on and the channel given. The **AMP** page has **SEL1 … SEL8** (the CC numbers), the **FLTR** page **VAL1 … VAL8** (their values). Open the EXPRESSION page with TRIG, then TRIG again.

Anything not listed stays at its default: THRU ON, V.SNS 100 %, every source to None.

| Track | Input channel | CHAN (out) | Other pages | EXPRESSION | What it tests |
| --- | --- | --- | --- | --- | --- |
| **A** | 9 | 1 | four trigs on steps 1, 5, 9, 13 (C4, velocity 100) | defaults: THRU ON, V.SNS 100 %, no routes | pass-through of pitch bend, aftertouch, mod wheel and breath; the sequencer |
| **B** | 10 | 2 | AMP: SEL1 = 74 | THRU ON, V.SNS 50 %; MW → CC:CC1 VAL (#74) +64.00; VEL → SRC:BC +32.00; AT → SRC:PITCH BEND +32.00 | a CC VAL route, velocity to breath before its note, aftertouch to pitch bend beside its own pass-through, V.SNS 50 % |
| **C** | 11 | 3 | AMP: SEL2 = 1; FLTR: VAL2 = 100 | THRU OFF; MW → CC:CC2 VAL (#1) −32.00 | a negative route merged with the track's own CC 1; THRU OFF |
| **D** | 12 | 4 | AMP: SEL3 = 71; FLTR: VAL3 = 100 | THRU ON, V.SNS 0 %; MW → SRC:PITCH BEND +64.00; AT → CC:CC3 VAL (#71) −64.00 | the wheel to pitch bend at full depth, a full negative route, V.SNS 0 % |
| **E** | 13 | 5 | — | THRU ON; MW → SRC:AFTERTOUCH +64.00 | one source added to another's pass-through |
| **F** | 14 | 6 | SRC: MW = 100 | THRU OFF, V.SNS 25 %; **AT** (knob G) → SRC:MOD WHEEL, A.DEP (knob H) −64.00; MW (knob C) stays None | a route taking the track's own wheel down; THRU OFF; V.SNS 25 % |
| **G** | 15 | 7 | AMP: SEL4 = 2 | THRU ON; AT → CC:CC4 VAL (#2) +32.00 | a route merged with breath's own pass-through on CC 2 |
| **H** | 16 | 8 | — | THRU OFF, no routes | nothing passes but the notes |

On B, C, D and G check that the CC VAL used is switched on: it shows a value on the FLTR page, not `--`.

## Audio tracks 1 to 8

Not measured by the program (their sound does not come out over MIDI), but set up so that saving, presets and the page are tested on every track, and for listening if you want to. Any sample; a sustained one (a pad) on track 1.

| Track | Input channel | EXPRESSION |
| --- | --- | --- |
| **1** | 1 | BEND 2.00; MW → FLTR:FREQUENCY +64.00; AT → AMP:VOLUME −64.00 (FLTR frequency low, AMP decay long) |
| **2** | 2 | BEND 12.00; VEL → AMP:VOLUME +32.00; V.SNS 50 % |
| **3** | 3 | BEND 0.50; AT → LFO1:DEPTH +64.00 (LFO1 on pitch) |
| **4** | 4 | BEND OFF; MW → SRC:SAMPLE LEVEL −64.00 |
| **5** | 5 | BEND 7.00; MW → FLTR:RESONANCE +16.00; V.SNS 0 % |
| **6** | 6 | MW → AMP:PAN +64.00; AT → AMP:PAN −64.00 |
| **7** | 7 | VEL → SRC:START +48.00 |
| **8** | 8 | defaults |

## Save

SAVE PROJECT AS: **EXPR TEST**. Then select MIDI track A.

## Running it

```
~/.venvs/dt/bin/python scripts/hwqual.py --list
~/.venvs/dt/bin/python scripts/hwqual.py --in "INTERFACE" --out "INTERFACE" --in2 "Digitakt" --out2 "Digitakt" --label full
```

INTERFACE is part of your MIDI interface's port name, as `--list` shows it. With only one connection, leave out `--in2` and `--out2`.

## The auto channel (optional, a separate run)

The auto channel needs a channel of its own, and all sixteen are taken above. For this run only: set track H's channel to OFF and AUTO CHANNEL to 16, then

```
~/.venvs/dt/bin/python scripts/hwqual.py --in "INTERFACE" --out "INTERFACE" --label auto --only auto
```

It asks you to select tracks A and B. Set track H back to 16 and AUTO CHANNEL to OFF afterwards.
