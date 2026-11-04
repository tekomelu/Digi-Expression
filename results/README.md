# Results

## stock-expression-os154.txt

`tests/stock-expression.txt` run on stock OS 1.54 (SHA-256 f78ba80f…3cf6) in digiemu c1b5735, from a snapshot with MIDI track A selected and its CHAN on at channel 1. Stock settings otherwise: AUTO CHANNEL 14, TRACK A CHANNEL 9, INPUT FROM MIDI+USB, RECEIVE NOTES and RECEIVE CC/NRPN ticked.

| Into MIDI IN | On the auto channel (14) | On track A's channel (9) |
| --- | --- | --- |
| Note on / off | out on channel 1 about 6 ms later; note off as note on with velocity 0 | the same |
| Pitch bend | dropped | dropped |
| Channel aftertouch | dropped | dropped |
| Mod wheel (CC1) | dropped | dropped |

This is the behaviour the mod changes. Times are emulated milliseconds.
