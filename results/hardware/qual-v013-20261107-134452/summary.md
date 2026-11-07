# MIDI Expression 0.13.0-experimental on a Digitakt mk1, OS 1.54

- Run: 2026-11-07 13:44:52, 7.2 minutes, label `v013`
- Firmware: midi-expression-0.13.0-experimental.syx
- Ports: in `Elektron Digitakt`, out `Elektron Digitakt`
- Result: 11 passed, 0 failed, 0 skipped

| Sent on | Phase | Check | Result | Note |
| --- | --- | --- | --- | --- |
| Elektron Digitakt | intro | a note into each MIDI track comes out on its channel | PASS |  |
| Elektron Digitakt | v013 | Press SETTINGS: EXPRESSION is the last row, below SYSTEM; YES on it opens the EXPRESSION page of the active track? | PASS |  |
| Elektron Digitakt | v013 | With the page up, TRK + trig 9 shows MID A, and TRK + trig 1 shows SMP 1 (the page follows the track)? | PASS |  |
| Elektron Digitakt | v013 | NO goes back to SETTINGS with the EXPRESSION row still selected; NO again closes SETTINGS as stock? | PASS |  |
| Elektron Digitakt | v013 | Open the page again and press TRIG: the page closes and the TRIG page shows, as stock? | PASS |  |
| Elektron Digitakt | v013 | With the page up, PLAY and STOP still start and stop the sequencer? | PASS |  |
| Elektron Digitakt | v013 | factory settings on MIDI track A: 8 notes out, no pitch bend, aftertouch, mod wheel or breath | PASS | notes 8, expression none |
| Elektron Digitakt | v013 | EXPR TEST reloaded: MIDI track A passes pitch bend again (THRU ON as saved) | PASS |  |
| Elektron Digitakt | v013 | In the new project, MIDI track A shows THRU OFF, and audio track 1 shows BEND OFF, no routes? | PASS |  |
| Elektron Digitakt | v013 | After that power cycle, audio track 1 shows V.SNS 100 % again (the change was not written while playing)? | PASS |  |
| Elektron Digitakt | v013 | After this power cycle, audio track 1 shows V.SNS 50 % (written once stopped)? | PASS |  |
