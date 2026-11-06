# Hardware tests

Everything to test on a unit before a release, and how. Most of it runs automatically: `scripts/hwqual.py` plays the keyboard on all eight MIDI tracks, records what the Digitakt sends over DIN and USB, and checks every value against a model of the firmware's sums. It starts and stops the sequencer itself. Where only eyes can tell, it asks yes / no / skip, and you can add a note to any answer.

## Before you start

1. Flash the exact build you are testing, and note its file name (the program asks for it).
2. Connect DIN (through a MIDI interface, both directions) and the Digitakt's USB port, both at once.
3. Install python-rtmidi once: `python3 -m venv ~/.venvs/dt && ~/.venvs/dt/bin/pip install python-rtmidi`.
4. Set up the test project **EXPR TEST** as [TEST-SETUP.md](TEST-SETUP.md) describes: the MIDI CONFIG settings, the eight MIDI tracks and the eight audio tracks.
5. Find the port names: `~/.venvs/dt/bin/python scripts/hwqual.py --list`.

## Runs

| Run | Command | Time | You do |
| --- | --- | --- | --- |
| 1. Everything, DIN and USB | `hwqual.py --in "INTERFACE" --out "INTERFACE" --in2 "Digitakt" --out2 "Digitakt" --label full` | about 30 min | load projects, one power cycle, answer the questions |
| 2. The auto channel (optional) | `hwqual.py --in "INTERFACE" --out "INTERFACE" --label auto --only auto` | 2 min | its own channel set-up (TEST-SETUP.md), select tracks when asked |
| 3. By ear (optional) | `--label ears --only ears` | 3 min | listen to audio track 1 |
| 4. The interface alone (optional) | `loopback.py --port "INTERFACE"` | 1 min | one MIDI cable from the interface's OUT to its own IN, no Digitakt |

Each run writes `results/hardware/qual-LABEL-DATE/`: `summary.md` (the table for the Modwerk hardware report), `report.json`, `log.txt` and `midi.jsonl` (every message on every port).

## What it checks

**On every MIDI track (automatic, sent first over DIN, then again over USB):**

- **tracks:** pitch bend, aftertouch, mod wheel and breath into each of the eight tracks, about 35 values each, against the model: pass-through, routes to CC VALs, to pitch bend, to aftertouch and to the mod wheel, positive and negative, merged with the track's own values and with other sources on the same CC, THRU OFF. Nothing else comes out on the track's channel, and the other tracks stay quiet.
- **notes:** 20 notes into each track: V.SNS 100, 50, 25 and 0 %, and velocity to breath sent before its note.
- **multi:** 3,000 random messages to all eight tracks, interleaved; every output matches the model afterwards.
- **latency:** 40 pitch bend round trips, timed to each output (DIN and USB).
- **seq:** the program starts the sequencer over MIDI; its notes go out and the live input stays exact.
- **DIN and USB:** in every phase, both outputs carry exactly the same messages: DIN in to DIN and USB out, then USB in to both.

**Playing (sent over DIN):**

- **clockloss:** 300 pitch bends to track A, each with a note to track H (THRU OFF: only stock passes it), with the clock stopped, running, and with the sequencer playing; then with the clock byte timed to land inside the pitch bend, and inside the note. On a DIN cable a clock byte may arrive in the middle of another message; if the note is lost as often as the pitch bend, the loss is in the interface or the stock input, not in the mod. Run 4 tells the interface apart.

- **flood:** 20 seconds at the MIDI wire rate to all eight tracks with the sequencer playing at 160 BPM: no stuck notes, every message well formed, the model exact again afterwards, and you check the screen keeps up.

**Saving:**

- **persist:** a probe of six tracks' settings before and after loading another project, a power cycle, an unsaved change and a reload, and after CREATE NEW (the default presets) and back; two questions on SAVE PROJECT AS and deleting a project.

**By eye:**

- **ui:** opening and closing the page; TRIG held; BEND's box, fine steps and snapping; depths and lists; the preset menu, its lists and the naming screen (repeats, delete, insert, the letter grid); LOAD, RESET, SELECT DEFAULT; old files; every audio track's settings after the power cycle; the MIDI LFO destinations; a value switched off; the track's own LFO and parameter locks.

## Not covered by the program

Write a line in the notes for each one you try:

- Several real synths at once on DIN and USB, played for a while.
- A long session: an hour or more of normal playing, saving and loading.
- Elektron Transfer: whether it can fetch and send the files in `/cfw/expression`.
