# MIDI Expression

Module version `0.10.0-experimental`, for the original Digitakt on OS 1.54, Core 2.1.

## Overview

Stock, the Digitakt passes an external keyboard's notes through a MIDI track to the synth it plays, but drops pitch bend, aftertouch and the mod wheel. With this mod those messages follow the notes and are **added to what the track itself sends**: on the AUTO CHANNEL to the active MIDI track, and on a track's TRACK CHANNEL to that track, on the track's CHAN out of MIDI OUT. A track's LFO bending its synth by a semitone and a live bend of a semitone give two. It is for live playing: nothing is recorded into the sequencer. Each track switches them on or off on its own EXPRESSION page; with them off (and an audio track's BEND OFF and no destinations) MIDI handling is stock.

## The EXPRESSION page (each track's own settings)

Select a track, press **TRIG** for its TRIG page, then **tap TRIG again**. The page looks like the stock FLTR and LFO pages: eight cells on knobs A–H, under the TRIG page's own header, track box and LEV meter. The knobs use the firmware's own acceleration, as the stock pages do (pushed, the stock push multiplier); **FUNC + turn** jumps between useful values, as FUNC + TUNE jumps octaves: depths −64, −48 … +48, +64; BEND OFF, 1, 2, 3, 5, 7, 12; V.SNS 0, 25, 50, 75, 100 %; destinations page by page (SRC, FLTR, AMP, LFO1, LFO2); CCs in tens. Turning a destination cell opens a list like the LFO page's DEST; pushing that knob closes it. TRIG or NO goes back; any other page or menu key does its stock job. The trig keys, TRK + trig, FUNC, PTN, BANK, the transport and LEVEL keep working.

| Knob | Audio track | Default | MIDI track | Default |
| --- | --- | --- | --- | --- |
| A | BEND: OFF, 1–12 semitones of TUNE | 2 | THRU: the input's PB, AT, MW and BC added to the track's own | ON |
| B | V.SNS: velocity sensitivity, 0–100 % | 100 % | V.SNS | 100 % |
| C | MW: the parameter the wheel moves | none | MW: the value the wheel moves | none |
| D | M.DEP: −64.00 to +64.00, as fine as LFO DEP | 0 | M.DEP | 0 |
| E | VEL: the parameter the played velocity moves | none | VEL | none |
| F | V.DEP | 0 | V.DEP | 0 |
| G | AT: the parameter aftertouch moves | none | AT | none |
| H | A.DEP | 0 | A.DEP | 0 |

**Audio destinations:** SRC Tune, Bit Reduction, Start, Length, Level; FLTR Frequency, Resonance, Env Depth, Attack, Decay, Sustain, Release and its second page; AMP Attack, Hold, Decay, Overdrive, Delay, Reverb, Pan, Volume; LFO1 and LFO2 Speed, Multiplier, Fade, Phase, Depth. Each adds to the track's own value, its parameter locks, its LFOs and Digi Matrix's routings, within the parameter's range, in the stock LFO's scale (±64 at full covers the whole range). The LFO ones are added before the LFO stage, everything else after it, which is where the engine reads them (measured).

**Velocity.** V.SNS scales the velocity of what the track plays, live from MIDI IN or from the sequencer: 100 % as played, 50 % plays 0–127 as 64–127, 0 % plays every note at full velocity. The sequencer records the velocity as played, so recorded notes keep their full range and V.SNS can be changed afterwards. As a source, VEL is the velocity of the last note the track played (live or from the sequencer, before V.SNS), held until its next note, so velocity routes play back from the sequencer as they were played live. On a MIDI track, V.SNS scales the velocity of the notes it sends to its synth.

**MIDI destinations** are the MIDI LFO page's: SRC Pitch Bend, Aftertouch, Mod Wheel, BC, and CC VAL 1–8 (FLTR page), each on the CC number its SEL picks (AMP page); the list and the box show that number ("CC:CC1 VAL #74"). After them, the track's own **LFO**: Speed, Multiplier, Fade, Start Phase and Depth (0.9.3), moved for as long as the source is, the LFO page untouched. Every one of these twelve values goes out as **the track's own value** (its knob, parameter locks and LFO) **plus everything live aimed at it** (THRU, and any route), capped to the MIDI range, as one message. Routes and THRU add up: the wheel routed to PB bends on top of the bend wheel; the wheel routed to MW with THRU on counts twice (M.DEP −64.00 cancels it).

**Two values on one CC number are merged.** The mod wheel is CC 1 and BC is CC 2; a CC VAL slot whose SEL is 1 or 2, or two slots with the same SEL, send on the same number, which on a stock Digitakt makes the synth jump between two values. While anything live is aimed at any of them, they go out as one CC: the track's own value is whichever of them changed last (what the synth heard last, as on a stock Digitakt), plus everything live aimed at any of them. With an LFO on one of them, that one keeps being "the last".

Changing a MIDI track's settings takes effect at once; values nothing is aimed at any more get the track's own value back, and are stock again.

**Saved with the project.** The settings are kept in a file of the mod's own on the +Drive (`/cfw/expression.a` and `.b`, next to Digi Slicer's files), because no byte of a saved sound or pattern is free for them (see docs/RESEARCH-OS154.md in the source repository):

- **SAVE PROJECT AS** (every project save) stores every track's settings under the project's name.
- **LOAD PROJECT** brings that project's settings back; a project saved without them gets the defaults.
- **At power-on** the settings come back as they were when the unit was switched off, as the Digitakt brings back its working project. Every change on the page is written a second later.

The file keeps the 32 projects saved most recently; two projects with the same name share their settings. Copying a sound or a track does not copy its EXPRESSION settings: they belong to the track's place (track 1–8, A–H) in the project.

## Before you play

PORT CONFIG > RECEIVE NOTES and, for the mod wheel and breath, RECEIVE CC/NRPN must be ticked. Nothing else to switch on: the mod works from power-on.

**Avoid a MIDI loop.** If the synth or a MIDI interface echoes the Digitakt's MIDI OUT back into its MIDI IN on the same channel, the mod's own output comes back as input and is added again, running up to the maximum (and notes double, as on a stock Digitakt). Keep the synth's MIDI thru/echo off, or on another channel.

### Quick tutorial: bend a synth through a MIDI track

1. Connect a keyboard to MIDI IN on the Digitakt's AUTO CHANNEL, and a synth to MIDI OUT.
2. Select MIDI track A (TRK + trig 9), switch its CHAN on (SRC page, FUNC + knob A) and set the synth's channel.
3. Play.
4. For an audio track: select it, TRIG, tap TRIG, and set BEND and the routes.

## Message map

| In at MIDI IN | Taken when | Out of MIDI OUT | Stock handling |
| --- | --- | --- | --- |
| Pitch bend (Ex), any channel | THRU ON | Ex on each routed MIDI track's CHAN: track's PB + (input − 8192), within 0–16383 | none (stock drops it) |
| Channel aftertouch (Dx) | THRU ON, or AT routed | Dx on each routed track's CHAN: track's AT + input, within 0–127 | none (stock drops it) |
| CC 1 (Bx 01) | THRU ON or MW routed, RECEIVE CC/NRPN ticked | Bx 01 on each routed track's CHAN: track's MW + input, within 0–127 | unchanged: the stock handler runs first, always |
| CC 2 (Bx 02) | THRU ON, RECEIVE CC/NRPN ticked | Bx 02: track's BC + input | unchanged: stock first |
| Note on | the track's VEL routed, or V.SNS below 100 % | the route's value just before the note; the note with V.SNS applied | the note as stock; recorded as played |
| Every other message | never | as stock | unchanged |

**Routing** is the stock note router's: a message on the AUTO CHANNEL goes to the active track only; otherwise to every track whose TRACK CHANNEL matches. Of those, only MIDI tracks A–H with CHAN on send anything. A message on the AUTO CHANNEL with an audio track selected is dropped, as stock.

**The track's own values.** A MIDI track sends its twelve values (knob, parameter locks, LFO, and the forced send at its first trig after a pattern change) as stock. While nothing live is aimed at a value, it goes out untouched. Once something is, the mod sends the sum instead of the track's value, so the two never fight.

**Leaving a track.** When the active track changes, what came in on the AUTO CHANNEL stops counting and the synth gets the track's own values back. A track reached through its own TRACK CHANNEL keeps the live value, as stock routing keeps its notes. A note's velocity stays (the sequencer's notes keep setting it).

## How it hooks in

- **Input:** the MIDI task hands each incoming message to one of sixteen handlers from a table, by type. Stock's pitch bend and channel aftertouch entries are an empty function. The mod points them at its own handlers and the control-change entry at a wrapper that calls the stock handler first. Three 4-byte pointer sites.
- **The track's own values:** an 8-byte site at the entry of the MIDI tracks' `send(track, channel, id, value, force)` lets the mod see and, while live input is active, take over the track's twelve LFO-page values (ids 21–32: PB, AT, MW, BC, CC VAL 1–8). Every other parameter passes untouched. The SEL numbers are read from the MIDI tracks' parameters in use (0x421c9188, as CC Recall documents).
- **Sending (0.9):** the mod never calls a MIDI driver. Each output channel keeps one pending flag per message type, and the value is worked out when it goes out. A change posts one wake record to the MIDI tracks' task; after its per-track pass, a 6-byte site at 0x400e1dbe (task.s) appends what is pending to the task's CC buffer, which the task then sends with its own midi_out, after the sequencer's notes. A velocity route goes into the notes buffer just before its note. Each buffer counts its bytes in one byte, so the mod fills it only to 128 bytes; the rest goes in the next pass. (0.8 called the DIN driver from the MIDI input and UI tasks, which froze a unit.)

- **Velocity:** an 8-byte site at 0x40077a60, right after the render stores a note's velocity for its voice, and a 6-byte site at the entry of the MIDI tracks' notes_append (0x400e1002) apply V.SNS to what plays (vel.s). The note router and recording are untouched.
- **The LFO slots:** an 8-byte site at the LFO stage's entry, 0x400ed766 (lfo.s).
- **A MIDI track's LFO:** an 8-byte site at the entry of the MIDI tracks' LFO function, 0x400ed438 (midilfo.s): the routes go into the LFO's words just before it runs and come out right after.
- **Saving:** a 6-byte site over each of the two sprintf calls that format the project jobs' "SAVING '%s'" (0x4004da62) and "LOADING '%s'" (0x4004fe28) messages tells the mod the project's name (store.s); the file is read and written in the UI tick with the firmware's own ekFS calls, as Digi Slicer does, only while the filesystem is free.
- **The page:** the core's key, encoder, draw and tick hooks only. Which page is up is read from the UI's view controller (measured, see docs/RESEARCH-OS154.md); nothing there is written.
- **Audio tracks:** a 6-byte site at 0x40077eea, the first call after the render's LFO stage that takes the parameter words (after Digi Matrix's and Digi Poly's sites), adds the live values to them.

All twelve sites are claimed in `build.json`; no other Modwerk Digitakt module patches them.

**With other mods** (`scripts/compat-pairs.sh`, `results/compat/pairs.txt`): it links with every other Modwerk Digitakt module except Digi Perform (the same TRIG gesture): CC Names, CC Recall, Digichain, Digi EQ, Digi Health, Digi Matrix, Digi Mono, Digi Neighbor, Digi Poly, Digi Slicer, Digi Sophie, Digi String, Digi Utils, Euclidean, FX Track, Input Prefader, MIDI Retrig, Note Accumulator, RAM Machines and Sample Tools. With Digi Slicer about 5 KB of RAM are left, so not every combination of three or more fits. Its CCs go through the task's own CC append, so FX Track sees them (a velocity route's CC goes straight in front of its note and does not). CC Recall's sends pass through it like the track's own. Digi Matrix's routings and its own add up on an audio track.

## Compatibility and limitations

- Built for OS 1.54 only.
- Saved per project name in a file of its own, not inside the project: copying a sound or track does not copy its settings, and two projects with the same name share them.
- Digi Perform opens its own page with the same gesture (TRIG again on the TRIG page), so the two are not built together.
- Sends where OUTPUT TO says (MIDI, USB or both), with the track's own values.
- Not recorded: the sequencer never sees the live values.
- A bend can reach the synth a few milliseconds before the note played with it: live notes go through the MIDI tracks' task (about 6 ms in the emulator), pass-through goes out in the same task's next pass (about 1 ms in the emulator).
- The track's PB, AT and MW knobs on screen show the track's own value, not the sum.
- Machines other than the sample ones take TUNE from their own SRC knob A.

## Tests

See [TESTING.md](TESTING.md). Host unit tests and the emulator for everything; on a unit, the MIDI-track stress test (0.9.2, 25 of 25) and the audio-track features (0.4–0.7). The MIDI LFO destinations and saving are not yet tested on a unit.

## Credits

Design and code: tekomelu. The page's knob feel, the firmware's knob graphic and name calls follow what Digi Perform documents (GPL-3.0-or-later); CC Recall (draadpiraat) documented the MIDI tracks' task. Licence: GPL-3.0-or-later.
