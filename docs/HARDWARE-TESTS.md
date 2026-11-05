# Hardware tests

Everything to test on a unit before a release, and how. Most of it runs automatically: `scripts/hwqual.py` plays the keyboard, records what the Digitakt sends and checks every value against a model of the firmware's sums. You set up one test project once, and the program asks you to press a key only where a test needs it. Where only ears or eyes can tell, it asks yes / no / skip, and you can add a note to any answer.

## Before you start

1. Flash the exact build you are testing, and note its file name (the program asks for it).
2. Connect the computer's MIDI OUT to the Digitakt's MIDI IN, and the Digitakt's MIDI OUT to the computer's MIDI IN. A synth is not needed: the computer listens in its place.
3. Install python-rtmidi once: `python3 -m venv ~/.venvs/dt && ~/.venvs/dt/bin/pip install python-rtmidi`.
4. Print the test project and set it up on the Digitakt: `~/.venvs/dt/bin/python scripts/hwqual.py --setup`. Save it as **EXPR TEST**.
5. Find the port names: `~/.venvs/dt/bin/python scripts/hwqual.py --list`.

## Runs

| Run | Command | Time | You do |
| --- | --- | --- | --- |
| 1. Everything over DIN | `hwqual.py --in "INTERFACE" --out "INTERFACE" --label din` | about 25 min | select a track a few times, PLAY / STOP, one power cycle, answer the questions |
| 2. The same over USB MIDI | `hwqual.py --in "Digitakt" --out "Digitakt" --label usb --only core,play` | about 5 min | the computer's USB cable to the Digitakt, PLAY / STOP |
| 3. With Digi Slicer (optional) | flash a build with both, then `--label slicer --only core,flood` | about 5 min | check Digi Slicer's own page still works |

Each run writes `results/hardware/qual-LABEL-DATE/`: `summary.md` (the table for the Modwerk hardware report), `report.json`, `log.txt` and `midi.jsonl` (every message both ways). Send the folders back.

## What each phase checks

**Automatic (core, about 3 minutes):**

- **thru:** MIDI track A, THRU ON. Pitch bend, aftertouch, mod wheel and breath, about 180 values, each added exactly to the track's own value and clamped; nothing on the other tracks' channels.
- **routes:** MIDI track B. Mod wheel to CC VAL1 on CC 74 at +64.00; aftertouch to pitch bend at +32.00, with aftertouch still passing beside it; velocity to breath at +32.00, sent before its note; V.SNS 50 % on every note's velocity.
- **merge:** MIDI track C, THRU OFF. The mod wheel at -32.00 on CC VAL2, whose SEL is CC 1: one CC 1, the track's own value minus the wheel; pitch bend, aftertouch and breath do not pass.
- **multi:** 1,500 random messages to A, B and C interleaved; afterwards each synth heard exactly the model's values, and nothing went to other channels.
- **latency:** 40 pitch bend round trips through track A (median and maximum, interface included).

**Playing (about 3 minutes; you select tracks and press PLAY / STOP):**

- **auto:** the auto channel plays the active track; on a track change the old track gets its own pitch bend back, and the new one takes the input.
- **seq:** with the sequencer playing, its notes go out and the wheel stays exact.
- **flood:** 20 seconds at the MIDI wire rate to A, B and C (notes, pitch bend, aftertouch, wheel, breath, clock) while the sequencer plays at 160 BPM; every note-on gets its note-off, the screen and keys stay responsive, and the routes are exact again afterwards.

**Saving (about 5 minutes; a power cycle):**

- **persist:** a short probe of track B and C's settings, before and after loading another project and back, after a power cycle, and after an unsaved change and a reload (the saved value comes back). Two questions: a project saved under a new name keeps its own settings, and deleting another project changes nothing.

**By ear (about 3 minutes):**

- **ears:** audio track 1 bends a whole tone each way, the wheel opens the filter, aftertouch turns the volume down, V.SNS 0 % plays soft and loud notes alike, and another audio track is not affected.

**By eye (about 10 minutes):**

- **ui:** the page opens and closes; TRIG held shows the knobs' values and letting go keeps the page, as on the stock pages (and a hold on the TRIG page does not open it); fine BEND and FUNC snapping to whole semitones; depths and destination lists; the preset menu (a one-second push, a twist cancels it); LEVEL one row a detent; FUNC typing; SAVE over a used slot asks; LOAD and RESET change the current track only; SELECT DEFAULT for new projects; audio and MIDI presets kept apart; an 0.11 file with BEND=5 loads as 5.00; the MIDI LFO destinations; a value switched off on its page; the track's own LFO and parameter locks with the live input added.

## Not covered by the program

Write a line in the notes for each one you try:

- Several real synths at once on DIN and USB, played for a while (the program checks the messages, not the synths).
- A long session: an hour or more of normal playing, saving and loading.
- Elektron Transfer: whether it can fetch and send the files in `/cfw/expression` (the preset sharing the README describes).
- A different OS than 1.54: not supported; the mod is built for 1.54 only.
