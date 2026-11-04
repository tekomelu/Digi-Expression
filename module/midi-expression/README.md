# MIDI Expression

Module version `0.5.0-experimental`, for the original Digitakt on OS 1.54, Core 2.1.

## Overview

Stock, the Digitakt passes an external keyboard's notes through a MIDI track to the synth it plays, but drops pitch bend, aftertouch and the mod wheel. With **SETTINGS > PB THRU**, **AT THRU** and **MW THRU** ticked, those messages follow the notes and are **added to what the track itself sends**: on the AUTO CHANNEL to the active MIDI track, and on a track's TRACK CHANNEL to that track, on the track's CHAN out of MIDI OUT. A track's LFO bending its synth by a semitone and a live bend of a semitone give two. It is for live playing: nothing is recorded into the sequencer. Unticked, MIDI handling is stock.

## The EXPRESSION page (each track's own settings)

Select a track, press **TRIG** for its TRIG page, then **tap TRIG again**. The page looks like the stock FLTR and LFO pages: eight cells on knobs A–H, under the TRIG page's own header, track box and LEV meter. The knobs use the firmware's own acceleration, as the stock pages do (pushed, the stock push multiplier); **FUNC + turn** puts a cell back to its default. Turning a destination cell opens a list like the LFO page's DEST. TRIG or NO goes back; any other page or menu key does its stock job. The trig keys, TRK + trig, FUNC, PTN, BANK, the transport and LEVEL keep working.

| Knob | Audio track | Default | MIDI track | Default |
| --- | --- | --- | --- | --- |
| A | BEND: OFF, 1–12 semitones of TUNE | 2 | PB: ON/OFF | ON |
| B | V.SNS: velocity sensitivity, 0–100 % | 100 % | AT: ON/OFF | ON |
| C | MW: the parameter the wheel moves | none | MW: ON/OFF | ON |
| D | M.DEP: −64 to +64 | +32 | V.SNS | 100 % |
| E | VEL: the parameter the played velocity moves | none | – | |
| F | V.DEP: −64 to +64 | +32 | – | |
| G | AT: the parameter aftertouch moves | none | MW>: CC 1 (added to the track's own MW) or CC 2–119 alone | CC 1 |
| H | A.DEP: −64 to +64 | +32 | AT>: channel aftertouch, or CC 1–119 alone | AT |

**Destinations:** SRC Tune, Bit Reduction, Start, Length, Level; FLTR Frequency, Resonance, Env Depth, Attack, Decay, Sustain, Release and its second page; AMP Attack, Hold, Decay, Overdrive, Delay, Reverb, Pan, Volume; LFO1 and LFO2 Speed, Multiplier, Fade, Phase, Depth. Each adds to the track's own value, its parameter locks, its LFOs and Digi Matrix's routings, within the parameter's range, in the stock LFO's scale (±64 at full covers the whole range). The LFO ones are added before the LFO stage, everything else after it, which is where the engine reads them (measured).

**Velocity.** V.SNS scales the velocity of notes from MIDI IN on their way to the track: 100 % as played, 50 % maps 0–127 to 64–127, 0 % plays every note at 127. It is what sounds and what the sequencer records. As a source, VEL is the last note's played velocity, held until the next note.

A CC chosen with MW> or AT> goes back to 0 when you leave the track. Changing a MIDI track's PB/AT/MW/MW>/AT> gives its synth the track's own values back.

The settings are held in RAM: they return to the defaults at every power-on and are not saved with the project.

## Controls in SETTINGS

| Control | What it does |
| --- | --- |
| PB THRU | Ticked: pitch bend from MIDI IN reaches the tracks the notes would play. Unticked: dropped, as stock. |
| AT THRU | Ticked: channel aftertouch. Poly aftertouch is not passed. |
| MW THRU | Ticked, and PORT CONFIG > RECEIVE CC/NRPN ticked: the mod wheel (CC 1), after the stock control-change handling. |

They start unticked at every power-on.

### Quick tutorial: bend a synth through a MIDI track

1. Connect a keyboard to MIDI IN on the Digitakt's AUTO CHANNEL, and a synth to MIDI OUT.
2. Select MIDI track A (TRK + trig 9), switch its CHAN on (SRC page, FUNC + knob A) and set the synth's channel.
3. Tick PB THRU, AT THRU and MW THRU in SETTINGS, and play.
4. For an audio track: select it, TRIG, tap TRIG, and set BEND and the routes.

## Message map

| In at MIDI IN | Taken when | Out of MIDI OUT | Stock handling |
| --- | --- | --- | --- |
| Pitch bend (Ex), any channel | PB THRU ticked | Ex on each routed MIDI track's CHAN: track's PB + (input − 8192), within 0–16383 | none (stock drops it) |
| Channel aftertouch (Dx) | AT THRU ticked | Dx on each routed track's CHAN: track's AT + input, within 0–127 | none (stock drops it) |
| CC 1 (Bx 01) | MW THRU and RECEIVE CC/NRPN ticked | Bx 01 on each routed track's CHAN: track's MW + input, within 0–127 | unchanged: the stock handler runs first, always |
| Every other message | never | as stock | unchanged |

**Routing** is the stock note router's: a message on the AUTO CHANNEL goes to the active track only; otherwise to every track whose TRACK CHANNEL matches. Of those, only MIDI tracks A–H with CHAN on send anything. A message on the AUTO CHANNEL with an audio track selected is dropped, as stock.

**The track's own values.** A MIDI track sends its PB, AT and MW (knob, parameter locks, LFO, and the forced send at its first trig after a pattern change) as stock. While no live input has reached a channel, those go out untouched. Once it has, the mod sends the sum on that channel instead of the track's value, so the two never fight.

**Leaving a track.** When the active track changes, every channel that AUTO CHANNEL input reached gets the track's own PB, AT and MW back, and the mod stops adding to them. A track reached through its own TRACK CHANNEL keeps the live value, as stock routing keeps its notes. Unticking a row does the same for that message.

## How it hooks in

- **Input:** the MIDI task hands each incoming message to one of sixteen handlers from a table, by type. Stock's pitch bend and channel aftertouch entries are an empty function. The mod points them at its own handlers and the control-change entry at a wrapper that calls the stock handler first. Three 4-byte pointer sites.
- **The track's own values:** an 8-byte site at the entry of the MIDI tracks' `send(track, channel, id, value, force)` lets the mod see and, while live input is active, take over the track's PB (id 21), AT (22) and MW (23) sends. Every other parameter passes untouched.
- **Priority:** each output channel keeps one pending flag per message type, and the value is worked out when it goes out. It goes through the stock DIN driver only while its transmit ring holds at most 24 bytes (about 8 ms of line time), so it never queues in front of a burst of sequencer notes. What waits goes with the next message or within one UI tick (33 ms).

- **Velocity:** a 6-byte site at 0x400d5d8e, the note router's call of the live note for each track, scales the velocity (vel.s).
- **The LFO slots:** an 8-byte site at the LFO stage's entry, 0x400ed766 (lfo.s).
- **The page:** the core's key, encoder, draw and tick hooks only. Which page is up is read from the UI's view controller (measured, see docs/RESEARCH-OS154.md); nothing there is written.
- **Audio tracks:** a 6-byte site at 0x40077eea, the first call after the render's LFO stage that takes the parameter words (after Digi Matrix's and Digi Poly's sites), adds the live values to them.

All seven sites are claimed in `build.json`; no other Modwerk Digitakt module patches them. It links beside Digi Matrix, Digi Poly, Digichain, FX Track and CC Recall together.

## Compatibility and limitations

- Built for OS 1.54 only.
- Not saved: the THRU rows start unticked and each track's settings at their defaults at every power-on.
- Digi Perform opens its own page with the same gesture (TRIG again on the TRIG page), so the two are not built together.
- DIN MIDI OUT only, not USB.
- Not recorded: the sequencer never sees the live values.
- A bend can reach the synth a few milliseconds before the note played with it: live notes go through the MIDI tracks' task (about 6 ms in the emulator), pass-through goes straight out.
- The track's PB, AT and MW knobs on screen show the track's own value, not the sum.
- Machines other than the sample ones take TUNE from their own SRC knob A.

## Tests

See [TESTING.md](TESTING.md). Emulator and host unit tests only; not yet tested on a unit.

## Credits

Design and code: tekomelu. The SETTINGS rows follow the row record and OS 1.54 addresses documented by FX Track (draadpiraat, GPL-3.0-or-later); CC Recall (draadpiraat) documented the MIDI tracks' task. Licence: GPL-3.0-or-later.
