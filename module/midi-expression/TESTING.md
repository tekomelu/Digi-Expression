# MIDI Expression testing

Module version 0.1.0-experimental, Digitakt I OS 1.54 (stock file SHA-256 `f78ba80f…3cf6`), Core 2.1 (`core-2.1-os1.54.elemod`). Built locally with Modwerk's `scripts/link-image.mjs` (commit ced6490), m68k-elf GCC 16.2.0 and binutils 2.47. Firmware, linked images and emulator state stay on the developer's computer.

## Evidence tier

Emulator only: digiemu c1b5735 with its six Unicorn patches, driven by `scripts/dtsession.py` in the project repository (digiemu's `Session`, measured mk1 key codes, digiemu's DIN MIDI in and out). No hardware result yet.

## How it works (OS 1.54)

See the README's "How it hooks in" and `docs/RESEARCH-OS154.md` in the project repository: the MIDI task's handler table at 0x401b8dd4, the note router 0x400d5c1c, the DIN driver 0x400028c6.

## Results

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

- **Priority against sequencer bursts and MIDI clock.** digiemu sends DMA buffers at once rather than at 31250 baud, so the transmit ring never fills and the priority gate never holds anything in the emulator. Clock and note timing under heavy aftertouch is for hardware.
- **Sequencer playback.** PLAY did not start the transport in these emulator runs, so no test played a pattern's MIDI trigs.
- **The UI tick resend** of held values (only reached when the ring is busy).
- **Panel after MIDI.** In digiemu c1b5735 panel keys stop taking effect once the MIDI task has handled any incoming message, on the stock OS too, so every routing case was set up with keys first and saved as a snapshot before MIDI was sent.
- USB input reaching MIDI OUT; OS 1.53; saving and loading projects; power cycles.
- CPU time per message and under a flood.

## Measured resources

Shared mod memory: 1,468 bytes (`ramSpare` 129,696 for the core alone, 128,228 with this module). Fast SRAM: none.

## Hardware steps for the author

1. Back up the unit's projects. Build `core 2.1 + midi-expression` with `link-image.mjs` from your own OS 1.54 and send it with Transfer. Recovery: hold FUNC at power-on, OS UPGRADE (TRIG 4), send the stock .syx.
2. Keyboard on the AUTO CHANNEL into MIDI IN, a synth on MIDI OUT; MIDI track A CHAN on the synth's channel. Tick the three rows.
3. Play with the wheels and pressure: the synth follows. Untick each row: that message stops.
4. Select an audio track: bends do nothing. Select another MIDI track: they follow it.
5. Priority: give MIDI tracks A-H 4-note chords on every step at 160 BPM or more, with CLOCK SEND on and a clock-following device, and sweep aftertouch hard. Compare note timing and the device's tempo stability with the rows ticked and unticked.
6. Check the panel stays responsive throughout, and that STOP and pattern changes leave no stuck notes.
