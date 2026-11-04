# MIDI Expression testing

Module version 0.3.0-experimental (image SHA-256 `33db5141…cfbc` built; PB RANGE capped at 12. The emulator results below were measured on `03c08b8e…ab3d`, which differs only in that cap, from the author's OS 1.54), Digitakt I OS 1.54 (stock file SHA-256 `f78ba80f…3cf6`), Core 2.1 (`core-2.1-os1.54.elemod`). Built locally with Modwerk's `scripts/link-image.mjs` (commit ced6490), m68k-elf GCC 16.2.0 and binutils 2.47. Firmware, linked images and emulator state stay on the developer's computer.

## Evidence tier

Emulator and host unit tests: digiemu c1b5735 with its six Unicorn patches, driven by `scripts/dtsession.py` in the project repository (digiemu's `Session`, measured mk1 key codes, digiemu's DIN MIDI in and out). No hardware result yet.

## How it works (OS 1.54)

See the README's "How it hooks in" and `docs/RESEARCH-OS154.md` in the project repository: the MIDI task's handler table at 0x401b8dd4, the note router 0x400d5c1c, the DIN driver 0x400028c6.

## Results for 0.3

| Test | Expected | Observed |
| --- | --- | --- |
| Host unit tests (`tests/host/test_expr.c`, now with `audio.c`) | TUNE scale and limits, PB RANGE held to 12, routes, caps, release, routing | 477 checks, 0 failures |
| TUNE's scale: CC 16 to track 1 at 64, 65, 88, 127, 0 on stock, reading the TUNE knob | — | 88 and 127 put the knob in the same place, so TUNE runs 40–88 (±24 semitones), one step a semitone: 256 per semitone in the parameter words |
| `scripts/probe-words.py`: track 1's parameter words at the call after the hook, PB RANGE 2, AT DEST L1.DEP +32, auto channel | TUNE 16384 ± 2 semitones; L1.DEP + 100 × 32 × 4; nothing after TRK + 2 | TUNE 16895 (full up, +511) and 15872 (full down, −512), 16384 at centre; L1.DEP 16384 → 29184 at AT 100; after TRK + 2 back to 16384 |
| The same, MW 127 to FLT.FREQ | + 127 × 32 × 4, within 32512 | the kit's FLT.FREQ was already 32512; it stayed at 32512 (capped) |
| SETTINGS rows | values drawn, LEFT/RIGHT step them | as expected (captures in `results/mod-0.3/`) |
| `tests/stock-expression.txt` and `tests/release-keys.txt` on 0.3 | as 0.2 | as 0.2 |
| 0.3 linked with Digi Matrix, Digichain, Digi Poly, FX Track and CC Recall | links, no site overlap | links |

Not tested: the sound of it (no samples on the emulator's fresh +Drive, so audio renders were silent), machines other than the sample ones, and CPU time in the audio interrupt.

## Results for 0.2

Logs in `results/mod-0.2/`.

| Test | Expected | Observed |
| --- | --- | --- |
| Host unit tests, `tests/host/test_expr.c` (`expr.c` compiled with stand-ins for the OS addresses) | sums, caps, take-over, release, priority, routing | 34 checks, 0 failures, on Linux x86-64 and on the Mac's Linux arm64 environment |
| `tests/stock-expression.txt` on 0.2 | identical to 0.1 (no track values yet, so the sum is the input) | identical |
| `tests/release.txt`: live PB, AT and CC 1 on the auto channel, then the active track changed (written to 0x4197c6b4) | the track's own values (PB centre, AT 0, MW 0) on the next UI tick | AT 0 and MW 0 sent only with the next incoming MIDI message, 240 ms later: in digiemu the UI task, and with it `ev_tick`, stops once MIDI has been handled (see Not tested), so the release came from the input path's own track check |
| The same with no MIDI after the change (`release-no-ui-tick.txt`) | — | nothing within 1.5 s: the UI tick did not run (before the driver fix below) |
| `tests/release-keys.txt`, with the driver fix: live PB, AT and CC 1 on the auto channel, then TRK + 1 (real keys), then TRK + 9, then the wheel | the track's own values within a UI tick; nothing on returning to A; A follows the next wheel move | as expected: `e0 00 40`, `d0 00`, `b0 01 00` 19–34 ms after the key; nothing on TRK + 9; `b0 01 20` on the wheel (`release-track-change-keys.txt`) |
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

- **Summing with a real LFO or parameter lock** on the unit, and how it sounds.

- **Priority against sequencer bursts and MIDI clock.** digiemu sends DMA buffers at once rather than at 31250 baud, so the transmit ring never fills and the priority gate never holds anything in the emulator. Clock and note timing under heavy aftertouch is for hardware.
- **Sequencer playback.** PLAY did not start the transport in these emulator runs, so no test played a pattern's MIDI trigs.
- **The UI tick resend** of held values (only reached when the ring is busy).
- **Panel after MIDI (resolved in the driver).** In digiemu c1b5735's headless `Session`, panel keys, redraw and the core's `ev_tick` stop once the MIDI task has handled any message, on the stock OS too. Cause, documented in digiemu's `emu/longrun.py` (`never_fake_set_posts`): the emulator force-satisfies semaphore waits, so the MIDI task's wait is forced through on every pass once a real message has woken it, the task spins at its priority and starves the UI. digiemu's app applies `never_fake_set_posts`; `Session` does not. `scripts/dtsession.py` now applies it, and keys work after MIDI on stock (`results/stock-keys-after-midi-os154.txt`). The 0.1 routing runs were made before the fix, with keys first and a snapshot.
- USB input reaching MIDI OUT; OS 1.53; saving and loading projects; power cycles.
- CPU time per message and under a flood.

## Measured resources

Shared mod memory: 5,612 bytes in 0.3 (`ramSpare` 124,084), 2,704 bytes in 0.2 (`ramSpare` 129,696 for the core alone, 126,992 with this module; 0.1 used 1,468). Fast SRAM: none.

## Hardware steps for the author

1. Back up the unit's projects. Build `core 2.1 + midi-expression` with `link-image.mjs` from your own OS 1.54 and send it with Transfer. Recovery: hold FUNC at power-on, OS UPGRADE (TRIG 4), send the stock .syx.
2. Keyboard on the AUTO CHANNEL into MIDI IN, a synth on MIDI OUT; MIDI track A CHAN on the synth's channel. Tick the three rows.
3. Play with the wheels and pressure: the synth follows. Untick each row: that message stops and the synth gets the track's own value back.
3b. Set the track's LFO to PB with a small depth and play a pattern: bend on top of it and listen for the sum, and for no jump back at the trigs. Push to the extremes: it must stop at the limit, never wrap.
3d. Select an audio track with a sustained sample: bend at PB RANGE 2 and check with a tuner (Digi utilities has one) that a full bend is two semitones. Set MW DEST to FLT.FREQ with the filter half closed and AT DEST to L1.DEP with LFO1 on: the wheel opens the filter, pressure deepens the LFO.
3c. Hold a bend and the mod wheel, select another track: the old synth returns to the track's values within about 33 ms; the new one follows only when you move a control.
4. Select an audio track: bends do nothing. Select another MIDI track: they follow it.
5. Priority: give MIDI tracks A-H 4-note chords on every step at 160 BPM or more, with CLOCK SEND on and a clock-following device, and sweep aftertouch hard. Compare note timing and the device's tempo stability with the rows ticked and unticked.
6. Check the panel stays responsive throughout, and that STOP and pattern changes leave no stuck notes.

## 0.4 (the EXPRESSION page)

- **Host unit tests:** 524 checks, 0 failures (`results/mod-0.4/host-unit-tests.txt`): per-track bend ranges and routes, per-track PB/AT/MW off, MW AS and AT AS on their own CC and back to 0 on leaving, a row change releasing the synth, and the page's keys and knobs (tap vs hold of TRIG, TRIG + REC, FUNC + TRIG, another page, overlay, cursor, limits).
- **Emulator** (`tests/page.txt`, captures in `results/mod-0.4/page/`): TRIG then a tap of TRIG opens the page on audio track 1 and MIDI track A; knobs A–E change the rows; TRIG goes back to the stock TRIG page; a 1 s hold of TRIG stays stock; FUNC + TRIG is the stock quantize page; TRK + 1 while the page is up shows track 1's settings; GLOBAL opens SETTINGS over it and the page is gone after NO; FLTR from the page gives the stock FLTR page.
- **Not yet on hardware.**

## 0.5 (stock-style page, velocity, LFO destinations)

- **Host unit tests:** 1955 checks, 0 failures (`results/mod-0.5/host-unit-tests.txt`): V.SNS mapping over every velocity and sensitivity, VEL as a source, LFO slots only at the LFO entry and the rest only after it, every cell's knob, limits, FUNC + turn defaults, the destination list, the accelerator safety net.
- **Emulator** (`results/mod-0.5/emulator-checks.txt`): which words the engine reads and when; a note of velocity 20 at V.SNS 50 % reaches the live note as 74; VEL → L1.DEP raises LFO1 depth at the LFO stage by 20 × 32 × 4 and is released on a track change. Page captures in `results/mod-0.5/page/`.
- **Not measurable in the emulator:** the knob feel. The firmware's accelerator returns 0 there for the stock pages too.
