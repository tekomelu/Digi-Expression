# digitakt-midi-expression

MIDI expression for the original Digitakt (mk1, OS 1.54): pitch bend, aftertouch and mod wheel pass-through to the MIDI tracks, pitch bend for the audio tracks and simple mod routes. A [Modwerk](https://modwerk.app/) module in development, kept private until it is ready.

Modwerk is an independent project, not affiliated with or endorsed by Elektron. This repository contains no Elektron firmware: everyone builds from their own official OS download, and stock code is referred to only by address, length and checksum.

## Plan

### Phase 1: pass-through to MIDI tracks A–H

(The original plan had SETTINGS rows PB THRU, AT THRU, MW THRU and THIN. As built, each track switches pitch bend, aftertouch and the mod wheel on its own EXPRESSION page, and coalescing plus the priority gate replace THIN; see the status below.)

- **Routing:** each message follows the route its notes would take. A TRACK A–H channel goes to that track; the auto channel goes to the selected track if it is a MIDI track with CHAN on. Output goes on the track's own CHAN, to the ports OUTPUT TO selects.
- **Priority:** the sequencer's notes, CCs and automation go out first. Expression waits in one slot per channel and type, newest value wins, and is sent only into free room in the output buffer, rate-limited.
- **Resets:** changing the selected track, STOP or a pattern change sends neutral values to the previous synth.
- **No snap-back:** while a live gesture is held, the track's own trig-time PB/AT/MW value is not resent.
- **Saving:** not saved yet; every setting starts at its default at power-on.

### Phase 2: pitch bend for the audio tracks

| Parameter | Values | What it does |
| --- | --- | --- |
| BEND RANGE | OFF, 1–24 semitones | How far incoming pitch bend moves an audio track's pitch |

A smooth pitch offset added where the engine applies parameters, the same place an LFO moves pitch. TUNE and the screen are untouched.

### Phase 3: simple mod routes

About four slots of SRC (MW / AT / PB), DEST (FLT FREQ, FLT RESO, LFO1 DEPTH, LFO2 DEPTH, perhaps AMP VOL) and AMOUNT (−64 to +64), applied to the track that receives the message. Digi Matrix already modulates these destinations, so this phase starts by talking to its author or proposing a shared core hook.

## Status

**Phase 1 works in the emulator** (4 November 2026): module `midi-expression` 0.7.0-experimental in [`module/midi-expression/`](module/midi-expression/), with its [README](module/midi-expression/README.md) and [TESTING.md](module/midi-expression/TESTING.md). Not yet tested on a unit.

- Version 1 sends to DIN MIDI OUT only, and leaves out the THIN setting: coalescing plus the priority gate replace it.
- 0.2: live input adds to the track's own PB/AT/MW (knob, locks, LFO) within the MIDI limits; leaving an auto-channel track restores its values. Host unit tests: `cc -std=c11 -DME_HOST_TEST -I tests/host tests/host/test_expr.c -o build/test_expr && build/test_expr`.
- 0.3: audio tracks: pitch bend moves TUNE by PB RANGE semitones; the mod wheel and aftertouch move a chosen sound parameter (MW/AT DEST, AMT). Links beside Digi Matrix and Digi Poly (a separate site after the LFO stage).
- 0.4: the EXPRESSION page, the TRIG page's second page (tap TRIG on the TRIG page): each track's own bend range and routes (audio) or PB/AT/MW on/off and MW/AT as any CC (MIDI). SETTINGS keeps the three THRU rows. Settings held in RAM, not saved yet. Not built together with Digi Perform (same gesture).
- 0.5: the page looks like the stock pages, stock knob acceleration, FUNC + turn to default, the LFO page's destination list; velocity as a source and V.SNS per track (sound and recording); LFO destinations work (a hook at the LFO stage's entry).
- 0.6: the list closes on a push of its knob; depths as fine as LFO DEP; FUNC + turn jumps between useful values; depths default to 0; the SETTINGS THRU rows are gone (each track's switches replace them, and the mod works from power-on).
- 0.7: the sequencer records the velocity as played; V.SNS applies to what plays, live or recorded; velocity routes follow sequencer notes too.
- Research notes: [docs/RESEARCH-OS154.md](docs/RESEARCH-OS154.md). Setup: [SETUP.md](SETUP.md).

### Build a test image

```sh
source env.sh
scripts/build-mod.sh "$PWD/build/midi-expression-0.7.0-experimental.syx"
```

The image is built from your own OS file and stays on your computer. Flash it with Elektron Transfer; to go back, hold FUNC at power-on, choose OS UPGRADE (TRIG 4) and send the stock .syx.

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).
