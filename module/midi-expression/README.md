# MIDI Expression

Module version `0.1.0-experimental`, for the original Digitakt on OS 1.54, Core 2.1.

## Overview

Stock, the Digitakt passes an external keyboard's notes through a MIDI track to the synth it plays, but drops pitch bend, aftertouch and the mod wheel. With **SETTINGS > PB THRU**, **AT THRU** and **MW THRU** ticked, those messages follow the notes: on the AUTO CHANNEL to the active MIDI track, and on a track's TRACK CHANNEL to that track, re-channelled to the track's CHAN and sent out of MIDI OUT. The sequencer's own notes and controls keep priority. Unticked, MIDI handling is stock.

## Controls

| Control | What it does |
| --- | --- |
| PB THRU | Ticked: pitch bend from MIDI IN goes out at its full 14-bit value on the CHAN of each MIDI track that notes on its channel would play. Unticked: dropped, as stock. |
| AT THRU | Ticked: channel aftertouch, routed the same way. Poly aftertouch is not passed. Unticked: dropped, as stock. |
| MW THRU | Ticked, and PORT CONFIG > RECEIVE CC/NRPN ticked: the mod wheel (CC 1), routed the same way, after the stock control-change handling. Unticked: CC 1 is handled as stock only. |

All three start unticked at every power-on and are not saved with the project.

## Usage

### Where to find it

SETTINGS, below SYSTEM: PB THRU, AT THRU and MW THRU. YES toggles a row, RIGHT ticks it, LEFT unticks it.

### Quick tutorial: bend a synth through a MIDI track

1. Connect a keyboard to MIDI IN, set it to the Digitakt's AUTO CHANNEL (SETTINGS > MIDI CONFIG > CHANNELS), and connect a synth to MIDI OUT.
2. Select MIDI track A (TRK + trig 9). On its SRC page press FUNC + knob A to switch CHAN on, and set it to the synth's channel.
3. Open SETTINGS and tick PB THRU, AT THRU and MW THRU.
4. Play the keyboard: notes, bends, pressure and the mod wheel reach the synth. Select another MIDI track and they follow it.

## Message map

| In at MIDI IN | Taken when | Out of MIDI OUT | Stock handling |
| --- | --- | --- | --- |
| Pitch bend (Ex), any channel | PB THRU ticked | Ex on each routed MIDI track's CHAN, same 14-bit value | none (stock drops it) |
| Channel aftertouch (Dx) | AT THRU ticked | Dx on each routed track's CHAN | none (stock drops it) |
| CC 1 (Bx 01) | MW THRU and RECEIVE CC/NRPN ticked | Bx 01 on each routed track's CHAN | unchanged: the stock handler runs first, always |
| Every other message | never | as stock | unchanged |

**Routing** is the stock note router's: a message on the AUTO CHANNEL goes to the active track only; otherwise to every track whose TRACK CHANNEL matches. Of those, only MIDI tracks A-H with CHAN on send anything. A message on the AUTO CHANNEL with an audio track selected is dropped, as stock.

## How it hooks in

The MIDI task hands each incoming message to one of sixteen handlers from a table, by message type. Stock's pitch bend and channel aftertouch entries are an empty function. This module points those two entries at its own handlers and the control-change entry at a wrapper that calls the stock handler first. Three 4-byte pointer sites, no stock instruction changed; claimed in `build.json`.

**Priority.** Each output channel keeps one waiting value per message type; a newer value replaces one not yet sent. Values go out through the stock DIN driver only while its transmit ring holds at most 24 bytes (about 8 ms of line time), so they never queue in front of a burst of sequencer notes. What waits goes with the next incoming message or within one UI tick (33 ms).

## Compatibility and limitations

- Built for OS 1.54 only.
- Not saved: the rows start unticked at every power-on.
- DIN MIDI OUT only, not USB: the stock USB sender is not safe to call from the MIDI input task.
- A MIDI track still sends its own PB, AT and MW values with its trigs, so a held bend jumps back at the track's next trig.
- Changing the selected track while bending leaves the previous synth at its last value.
- Links and runs beside FX Track and CC Recall (emulator). No site overlap with any Modwerk Digitakt module checked so far.

## Tests

See [TESTING.md](TESTING.md). Emulator only; not yet tested on a unit.

## Credits

Design and code: tekomelu. The SETTINGS rows follow the row record and OS 1.54 addresses documented by FX Track (draadpiraat, GPL-3.0-or-later). Licence: GPL-3.0-or-later.
