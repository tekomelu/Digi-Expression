# digitakt-midi-fun

MIDI expression for the original Digitakt (mk1, OS 1.54): pitch bend, aftertouch and mod wheel pass-through to the MIDI tracks, pitch bend for the audio tracks and simple mod routes. A [Modwerk](https://modwerk.app/) module in development, kept private until it is ready.

Modwerk is an independent project, not affiliated with or endorsed by Elektron. This repository contains no Elektron firmware: everyone builds from their own official OS download, and stock code is referred to only by address, length and checksum.

## Plan

### Phase 1: pass-through to MIDI tracks A–H

A SETTINGS page with:

| Parameter | Values | What it does |
| --- | --- | --- |
| PB THRU | on / off | Forward incoming pitch bend at full 14-bit resolution |
| AT THRU | on / off | Forward channel aftertouch |
| MW THRU | on / off | Forward the mod wheel (CC1) |
| THIN | OFF / LOW / HIGH | How hard repeated values are thinned when the DIN line is busy; default LOW |

- **Routing:** each message follows the route its notes would take. A TRACK A–H channel goes to that track; the auto channel goes to the selected track if it is a MIDI track with CHAN on. Output goes on the track's own CHAN, to the ports OUTPUT TO selects.
- **Priority:** the sequencer's notes, CCs and automation go out first. Expression waits in one slot per channel and type, newest value wins, and is sent only into free room in the output buffer, rate-limited.
- **Resets:** changing the selected track, STOP or a pattern change sends neutral values to the previous synth.
- **No snap-back:** while a live gesture is held, the track's own trig-time PB/AT/MW value is not resent.
- **Saving:** off at power-on, not saved, like FX Track and CC Recall.

### Phase 2: pitch bend for the audio tracks

| Parameter | Values | What it does |
| --- | --- | --- |
| BEND RANGE | OFF, 1–24 semitones | How far incoming pitch bend moves an audio track's pitch |

A smooth pitch offset added where the engine applies parameters, the same place an LFO moves pitch. TUNE and the screen are untouched.

### Phase 3: simple mod routes

About four slots of SRC (MW / AT / PB), DEST (FLT FREQ, FLT RESO, LFO1 DEPTH, LFO2 DEPTH, perhaps AMP VOL) and AMOUNT (−64 to +64), applied to the track that receives the message. Digi Matrix already modulates these destinations, so this phase starts by talking to its author or proposing a shared core hook.

## Status

Setup. Nothing built or tested yet.

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).
