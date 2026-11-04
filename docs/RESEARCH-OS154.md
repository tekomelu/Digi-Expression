# OS 1.54 research notes: the MIDI input path

Digitakt mk1 OS 1.54, stock file SHA-256 f78ba80f…3cf6, MAIN OS image loaded at 0x40000400. Found in digiemu c1b5735 with `scripts/trace-midi-in.py` and m68k-elf-objdump on the user's own image. Addresses and behaviour only; no firmware bytes are kept here.

## 1. From the jack to the MIDI task

As FX Track's TESTING.md describes: the MIDI input interrupt parses each message and posts it to the MIDI task with `queue_send` (0x40001b7a), as a length and a pointer, to the DIN queue (0x421a9d7c) or the USB queue (0x421a9d5c), and wakes the task (semaphore 0x421a9d34).

## 2. The MIDI task's dispatcher (0x400d48d6–0x400d49f2)

A loop that takes messages from the DIN queue and the USB queue in turn. For each message, at 0x400d4920 (DIN; 0x400d498a for USB):

- reads the status byte; type = status >> 4, channel = status & 15;
- drops it unless INPUT FROM (0x4197c754; bit 0 MIDI, bit 1 USB) includes the port, except 0xF0;
- sets a flag: DIN 0x10 if channel == AUTO CHANNEL (0x4197c6fc, 0–15), else 8; USB 4 or 2;
- calls `handler[type](msg, len, flag)` from a 16-entry table at **0x401b8dd4**.

| Type | Handler | What it does |
| --- | --- | --- |
| 0x0–0x7 | 0x400d44a4 | `rts` (not status bytes) |
| 0x8 note off | 0x400d5dae | if RECEIVE NOTES: note-off router 0x400d5ae0(channel, note, …) |
| 0x9 note on | 0x400d5dd2 | if RECEIVE NOTES: velocity 0 → note off; else router 0x400d5c1c(channel, note, velocity, flag) |
| 0xA poly aftertouch | 0x400d44a6 | **`rts`: dropped** |
| 0xB control change | 0x400d4cb8 | CC handling (parameters, FX CONTROL CH); CC1 is read but nothing goes out |
| 0xC program change | 0x400d4e34 | program change |
| 0xD channel aftertouch | 0x400d44a8 | **`rts`: dropped** |
| 0xE pitch bend | 0x400d44aa | **`rts`: dropped** |
| 0xF system | 0x400d44ac | second table at 0x401b8d30, by the low nibble |

Trace (`results/stock-expression-os154.txt` setup): a note is read at 0x400d4920 and then by the note handler; pitch bend and aftertouch are read once at 0x400d4920 and never again; CC1 continues into 0x400d4cb8.

**For the mod:** the table is data. Pointing entries 0xD and 0xE at the mod's own handlers is a 4-byte pointer site each, the kind Digi Poly already uses, with no instruction patched. CC1 can be handled by pointing entry 0xB at a wrapper that handles CC1 and then calls the stock 0x400d4cb8 for everything.

## 3. Note routing (0x400d5c1c)

Arguments: channel, note, velocity, flag.

- **Auto mode** when channel == AUTO CHANNEL (or channel < 0): the destination is only the **active track**, 0x4197c6b4 (0–15: tracks 1–8, then MIDI tracks A–H). A track whose TRACK CHANNEL equals the auto channel is not played (auto wins).
- **Otherwise**: every track t in 0–15 whose TRACK CHANNEL, the 16-long table at **0x4197c700**, equals the channel.
- Per selected track it calls three predicates (0x4007851a, 0x40078528, 0x4007853a; to identify) and then the live note-on 0x400d5604(track, note, velocity, flag, 0, −1, −1).
- 0x4219ccd8 (16 or more longs, indexed by note) remembers which track played a note, so the note off goes to the same track.

**For the mod:** pitch bend, aftertouch and CC1 should take exactly this routing: auto → the active track; otherwise tracks whose TRACK CHANNEL matches; keep MIDI tracks (8–15) with CHAN on. CC Recall documents `channel(track)` at 0x400e09d8 (−1 when CHAN is off).

## 4. Control change routing (0x400d4cb8)

Gated by RECEIVE CC/NRPN (0x4197c77c). Same routing as notes: if the channel is FX CONTROL CH (0x4197c740), the effects handler 0x400d6c80(0x10, cc, value, auto, 0); then 0x400d6c80(track, cc, value, auto, 0) for the active track on the auto channel, or for every track whose TRACK CHANNEL matches.

## 5. From a live note to MIDI OUT

Traced with `scripts/trace-calls.py` (a note on the auto channel, MIDI track A selected):

1. The MIDI input task (stack around 0x421a90xx): router → live note 0x400d5604(track 8, note, velocity, flag 0x10, 0, −1, −1).
2. The live note posts a record to the **MIDI tracks' task**: a 20-byte slot in a 64-entry ring at 0x421b5210 (type word 1 for a note, +16 a pointer to an event from the pool 0x400e0cb0), sent with `queue_send(0x421b51f0, slot)` at 0x400e1fb6. Other record types are posted at 0x400e0cde (type 4), 0x400e0d1e (4) and 0x400e0d56 (5).
3. The MIDI tracks' task (stack around 0x421cd7xx), about 6 ms later: 0x400e1dd4 → **midi_out(len, bytes)** at 0x400e0992.
4. midi_out sends to DIN when OUTPUT TO (0x421c9560) bit 0 is set, through **din_send(len, bytes)** at 0x400028c6, and to USB when bit 1 is set, through 0x40004990.

Other functions of the MIDI tracks' task: append(len, bytes) 0x400e0dac copies into a buffer at 0x421b6a22 with its length byte at 0x421b6a21 and no locking, so only the MIDI tracks' task may call it. channel(track) 0x400e09d8 returns the MIDI track's CHAN, or −1 when off (as CC Recall documents).

## 6. The DIN transmit driver

- **din_send(len, bytes), 0x400028c6.** A 4 KB ring at *(0x4060f7d4), write index 0x4060f7e4, bytes not yet handed to the DMA at 0x4060f7e8, DMA-busy flag 0x4060f7cc, DMA position from eDMA registers 0xfc0454b4 and 0xfc0454bc and 0x4060f7e0/0x4060f7ec. It first busy-waits until the ring has room for the message, then copies and starts DMA channel 37 with **all interrupts masked** (SR = 0x2700). So it is safe from any task, one whole message per call, but it spins when the ring is full: the mod checks the free space the same way before calling, and only sends while the ring is nearly empty.
- **Single bytes, 0x400029fa.** Writes the UART data register directly when idle, otherwise queues to a 1 KB byte queue (count 0x4060f7f8) that UART9's interrupt drains. This is the path for single real-time bytes.
- **USB, 0x40004990.** Busy-waits on its own ring (0x40610478 against 4091) with no interrupt lock, so it is only safe from the MIDI tracks' task. Version 1 of the mod sends to DIN only.

## Open

- Where MIDI clock (0xF8) is sent from, and its timing on hardware under heavy aftertouch: digiemu sends DMA buffers at once rather than at 31250 baud, so DIN timing can only be judged on the unit.
- What the three predicates in the note router (0x4007851a, 0x40078528, 0x4007853a) check.
- Resets on track change and STOP, and holding back the track's own trig-time PB/AT/MW while a live gesture is held (version 2).
