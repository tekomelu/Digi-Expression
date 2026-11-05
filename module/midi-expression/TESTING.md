# MIDI Expression testing

Version 0.10.0-experimental, Digitakt I OS 1.54 (stock file SHA-256 f78ba80f…3cf6), Core 2.1.

## Evidence tier

`none` in Modwerk's terms: the author's own tests below, on the host, in the emulator (digiemu, pinned c1b5735) and on one unit. No independent report yet.

## How to run the tests

All commands from the source repository's root, with the build environment of its BUILDING.md.

| What | Command | Result (0.10.0) |
| --- | --- | --- |
| Host unit tests: expr.c, audio.c, page.c and store.c compiled for the computer with stand-ins for the firmware | `cc -std=c11 -Wall -Wextra -DME_HOST_TEST -I tests/host tests/host/test_expr.c -o build/test_expr && build/test_expr` | 2,394,668 checks, 0 failures; the same under `-fsanitize=address,undefined` (`results/mod-0.10/host-unit-tests.txt`) |
| MIDI tracks on the firmware: odd combinations of live input, routes, merged CC numbers, the track's own sends and track changes, checked against a model of the sums | `scripts/emu-midi-whacky.py` | 330 checks, 0 failures (`emu-midi-whacky.txt`) |
| Saving: SAVE PROJECT AS, LOAD PROJECT of the saved project and of one saved without the mod, and a power-on (the mod's RAM cleared, the file read back from the card), all through the firmware's own menus and +Drive code | `scripts/emu-store.py` | 7 checks, 0 failures (`emu-store.txt`) |
| Latency of live pitch bend through MIDI track A | `scripts/emu-latency.py` | on MIDI OUT within 1 ms, 20 of 20 (`latency.txt`) |
| A track's own value from its parameters before it sends one | `scripts/emu-own-params.py` | MW 10 + wheel 50 = CC 1 60; PB 12288 + 128 (`emu-own-params.txt`) |
| The MIDI LFO destinations leave the track's parameters as they were | `scripts/emu-midi-lfo.py` | 0 failures (`emu-midi-lfo.txt`); whether the LFO moves is for a unit: digiemu cannot PLAY |
| Links with each other Modwerk Digitakt module | `scripts/compat-pairs.sh MODULE ...` | every one but Digi Perform (`results/compat/pairs.txt`) |
| On a unit: expression, notes and floods into MIDI IN, everything from MIDI OUT recorded and checked | `scripts/hwping.py PORT`, then `scripts/hwstress.py --in PORT --out PORT` | 0.9.2: 25 of 25 (`results/hardware/stress-20261105-135203`) |

Emulator scripts take the image's settings from `toolchain/emu-NAME.env` (`scripts/emu-image.sh NAME` builds and sets one up): `set -a; . toolchain/emu-NAME.env; set +a; digiemu/.venv/bin/python scripts/SCRIPT.py`. Screens for the documentation: `scripts/emu-screens.py`.

## What the host tests cover

Routing (auto channel, TRACK CHANNEL, CHAN off, audio tracks); sums within the MIDI limits for THRU and every route; release on a track change; merging of values on one CC number (MW with a CC VAL slot on SEL 1, BC with SEL 2, two slots on one SEL, SEL changed while merged); a fuzz of 300 rounds of 60 random events with every message valid and everything back to stock at the end; only the MIDI tracks' task sends, one wake queued at most, the buffer never filled past its room; unchanged values not sent again, changes and forced sends always; the track's own value from its parameters until it sends one; velocity sensitivity for audio and MIDI tracks, recorded velocity untouched, VEL as a source; the audio render's routes and TUNE; the MIDI LFO routes in and out of the LFO's words; the page's keys, knobs, lists, FUNC jumps and drawing; the store: waits for the drive, written a second after a change, saved and loaded per project name, defaults for an unknown project, power-on, a torn newer file (the older one is read), out-of-range values in a file, a failed write retried, more projects than records.

## On a unit

- **0.4–0.7, audio tracks** (internal sounds, by the author): bend, routes, the page and its knobs, velocity sensitivity, the LFO destinations of audio tracks.
- **0.8, MIDI tracks:** froze the unit. 0.8 sent MIDI itself from the MIDI input and UI tasks; on hardware that held everything back or let a second task into the DIN driver. 0.9 sends only through the MIDI tracks' own task.
- **0.9.0–0.9.2:** `hwping.py` showed each value once, as expected, after 0.9.1 stopped resending unchanged values. `hwstress.py` on 0.9.2: 25 of 25 (own values, THRU on 200 random PB/AT/MW/BC, MW and VAL1 merged on CC 1, release and return, wheel to CC 74, pressure to PB −64, velocity to BC before its note, V.SNS 50 % live and from the sequencer, an LFO on MW merged with the wheel, recording as played). Measured round trip through a USB MIDI interface: live notes 12 ms median, pitch bend 11 ms, CC 1 4 ms. The flood phase was left out (the interface hung under it).
- **Not yet on a unit:** saving with the project (0.10), the MIDI LFO destinations (0.9.3), the flood phase.

## Measured resources

RAM: about 10.5 KB above 0.9.3, most of it the store (33 records of 212 bytes). In a test image alone, 97 KB of the module area stay free; with Digi Slicer, 5.5 KB (`results/compat/pairs.txt`). CPU: not measured; the audio render does per-track additions only for tracks with something live, and the store works in the UI tick.

## History

The research behind each version is in docs/RESEARCH-OS154.md; the changelog is module/changelog/digitakt-midi-expression.json; the results of earlier versions are in the repository's history.
