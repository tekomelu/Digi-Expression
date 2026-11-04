# MIDI Expression testing

Module version 0.2.0-experimental (image SHA-256 `13f93214…87a3` built from the author's OS 1.54), Digitakt I OS 1.54 (stock file SHA-256 `f78ba80f…3cf6`), Core 2.1 (`core-2.1-os1.54.elemod`). Built locally with Modwerk's `scripts/link-image.mjs` (commit ced6490), m68k-elf GCC 16.2.0 and binutils 2.47. Firmware, linked images and emulator state stay on the developer's computer.

## Evidence tier

Emulator and host unit tests: digiemu c1b5735 with its six Unicorn patches, driven by `scripts/dtsession.py` in the project repository (digiemu's `Session`, measured mk1 key codes, digiemu's DIN MIDI in and out). No hardware result yet.

## How it works (OS 1.54)

See the README's "How it hooks in" and `docs/RESEARCH-OS154.md` in the project repository: the MIDI task's handler table at 0x401b8dd4, the note router 0x400d5c1c, the DIN driver 0x400028c6.

## Results for 0.2

Logs in `results/mod-0.2/`.

| Test | Expected | Observed |
| --- | --- | --- |
| Host unit tests, `tests/host/test_expr.c` (`expr.c` compiled with stand-ins for the OS addresses) | sums, caps, take-over, release, priority, routing | 34 checks, 0 failures, on Linux x86-64 and on the Mac's Linux arm64 environment |
| `tests/stock-expression.txt` on 0.2 | identical to 0.1 (no track values yet, so the sum is the input) | identical |
| `tests/release.txt`: live PB, AT and CC 1 on the auto channel, then the active track changed (written to 0x4197c6b4) | the track's own values (PB centre, AT 0, MW 0) on the next UI tick | AT 0 and MW 0 sent only with the next incoming MIDI message, 240 ms later: in digiemu the UI task, and with it `ev_tick`, stops once MIDI has been handled (see Not tested), so the release came from the input path's own track check |
| The same with no MIDI after the change (`release-no-ui-tick.txt`) | — | nothing within 1.5 s: confirms the UI tick does not run in digiemu after MIDI |
| 0.2 linked with FX Track and CC Recall | links | links; not run in the emulator |

Not reachable in digiemu and covered only by the host tests: a track's own PB/AT/MW sends (they need the sequencer to play, a parameter lock or an LFO) added to live input, and the cap at the MIDI limits.

## Results for 0.1

Logs are in `results/mod-0.1/` of the project repository. Setup unless stated: AUTO CHANNEL 14, TRACK A CHANNEL 9, TRACK B CHANNEL 10, MIDI track A selected with CHAN on (channel 1), the three rows ticked.

| Test | Expected | Observed |
| --- | --- | --- |
| `tests/stock-expression.txt` on stock OS 1.54 | notes out, PB/AT/CC1 dropped | as expected (`results/stock-expression-os154.txt`) |
| The same on this build, rows ticked | notes as stock; PB (3 values), AT and CC1 out on channel 1, from the auto channel and from track A's channel 9 | as expected; PB values unchanged (`e0 00 60`, `e0 7f 7f`, `e0 00 40`) |
| The same on this build, rows unticked | identical to stock | identical: the four note messages only |
| PB on channel 3 (audio track 3 only) | nothing | nothing |
| PB on channel 10 (track B, CHAN off) | nothing | nothing |
| Audio track 1 selected: PB, AT, CC1 on the auto channel | nothing | nothing |
| Audio track 1 selected: PB on track A's channel 9 | out on A's CHAN | out |
| Track A selected, CHAN off: PB on the auto channel and on channel 9 | nothing | nothing |
| Track B selected, CHAN on (A off): PB on the auto channel and on channel 10 | out on B's CHAN | out; PB on channel 9 (A off): nothing |
| `tests/cc-sweep.txt`: every controller 1-127 except bank, data-entry, NRPN/RPN and channel-mode, on channels 9 and 14 | stock: whatever stock sends; this build: the same plus CC 1 | stock sent nothing; this build sent exactly two messages, `b0 01 40` (CC 1 from each channel) |
| `tests/flood.txt`: 400 pitch bends, 200 aftertouch, 200 CC 1 at 1 ms spacing, notes between, then final values | no hang; notes out; the final values out last | no hang; 2214 bytes in, 2214 out; both notes out; final `e0 00 40`, `d0 00`, `b0 01 00` last |
| This build with FX Track and CC Recall, CC RECALL, FX TRACK and PB THRU ticked, AT and MW unticked | PB passes, AT and CC1 do not | as expected |
| SETTINGS rows | three checkbox rows below SYSTEM; YES toggles | as expected (`media/settings-off.png`, `media/settings-on.png`) |

## Not tested

- **The UI tick on hardware.** Track-change release and unticked rows rely on `ev_tick` (30 Hz). It could not be observed in digiemu after MIDI.
- **Summing with a real LFO or parameter lock** on the unit, and how it sounds.

- **Priority against sequencer bursts and MIDI clock.** digiemu sends DMA buffers at once rather than at 31250 baud, so the transmit ring never fills and the priority gate never holds anything in the emulator. Clock and note timing under heavy aftertouch is for hardware.
- **Sequencer playback.** PLAY did not start the transport in these emulator runs, so no test played a pattern's MIDI trigs.
- **The UI tick resend** of held values (only reached when the ring is busy).
- **Panel after MIDI.** In digiemu c1b5735 panel keys stop taking effect once the MIDI task has handled any incoming message, on the stock OS too, so every routing case was set up with keys first and saved as a snapshot before MIDI was sent.
- USB input reaching MIDI OUT; OS 1.53; saving and loading projects; power cycles.
- CPU time per message and under a flood.

## Measured resources

Shared mod memory: 2,704 bytes in 0.2 (`ramSpare` 129,696 for the core alone, 126,992 with this module; 0.1 used 1,468). Fast SRAM: none.

## Hardware steps for the author

1. Back up the unit's projects. Build `core 2.1 + midi-expression` with `link-image.mjs` from your own OS 1.54 and send it with Transfer. Recovery: hold FUNC at power-on, OS UPGRADE (TRIG 4), send the stock .syx.
2. Keyboard on the AUTO CHANNEL into MIDI IN, a synth on MIDI OUT; MIDI track A CHAN on the synth's channel. Tick the three rows.
3. Play with the wheels and pressure: the synth follows. Untick each row: that message stops and the synth gets the track's own value back.
3b. Set the track's LFO to PB with a small depth and play a pattern: bend on top of it and listen for the sum, and for no jump back at the trigs. Push to the extremes: it must stop at the limit, never wrap.
3c. Hold a bend and the mod wheel, select another track: the old synth returns to the track's values within about 33 ms; the new one follows only when you move a control.
4. Select an audio track: bends do nothing. Select another MIDI track: they follow it.
5. Priority: give MIDI tracks A-H 4-note chords on every step at 160 BPM or more, with CLOCK SEND on and a clock-following device, and sweep aftertouch hard. Compare note timing and the device's tempo stability with the rows ticked and unticked.
6. Check the panel stays responsive throughout, and that STOP and pattern changes leave no stuck notes.
