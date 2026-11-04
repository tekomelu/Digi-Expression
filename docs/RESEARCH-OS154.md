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

## Open

- How a live note reaches a MIDI track's output (0x400d5604 → the MIDI tracks' task) and which context may append to the MIDI tracks' output buffer (0x400e0dac): FX Track found that posting from the wrong task stalled the panel.
- Whether MIDI clock goes through that buffer (the priority question).
- What the three predicates in the router check.
