// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI Expression for the Digitakt mk1, OS 1.54: pitch bend, channel
// aftertouch and the mod wheel arriving at MIDI IN are added to what a MIDI
// track sends its synth, on the route the notes take. For live playing only:
// nothing is recorded into the sequencer.
//
// Input. Stock, the MIDI task hands each incoming message to one of sixteen
// handlers, chosen by its type, from a table at 0x401b8dd4: message, length
// and a flag (0x10 on the auto channel, 8 otherwise; 4 and 2 from USB). The
// entries for channel aftertouch (0xD) and pitch bend (0xE) are an empty
// function, and the control-change handler (0xB) does not forward CC 1. This
// mod points the D and E entries at its own handlers and wraps the B entry
// (the stock handler runs first, always).
//
// Routing is the stock note router's (0x400d5c1c): on the auto channel, the
// active track only; otherwise every track whose TRACK CHANNEL matches. Of
// those, MIDI tracks A-H with CHAN on take the message, on their CHAN.
//
// Combining. A MIDI track sends its own PB, AT and MW (its knob, parameter
// locks and LFO) through send(track, channel, id, value, force) at
// 0x400e0ddc: id 21 pitch bend (a 14-bit value), 22 channel aftertouch and
// 23 CC 1 (8.8 fixed point, the MIDI value in the top byte). A 8-byte site at
// its entry (send.s) calls me_filter first, which remembers the track's
// value per channel. What goes out on a channel is then:
//   pitch bend  = track + (live - 8192), within 0..16383
//   aftertouch  = track + live,          within 0..127
//   mod wheel   = track + live,          within 0..127
// With no live input on a channel (or the row unticked), the track's sends go
// out exactly as stock. Once live input has touched a channel, the track's
// own PB/AT/MW sends there are taken over and the combined value is sent by
// this mod instead, so the two never race.
//
// Leaving. When the active track changes, a channel that auto-channel input
// touched gets the track's own values back and is released, as if this mod
// had never been there. Unticking a row does the same for that message type.
//
// Sending. Each channel has one pending flag per type; the value is computed
// when it goes out. It goes through the stock DIN driver (din_send, which
// masks interrupts while it copies into its 4 KB transmit ring) only while
// that ring holds at most PRIORITY_BYTES, so expression never queues in front
// of a burst of sequencer notes; what waits goes with the next message or the
// UI's 30 Hz tick. DIN only; USB is not sent to.

#include <stdint.h>

#ifdef ME_HOST_TEST
// Host unit tests (tests/host/): the test file defines these, and includes
// this file, so the logic runs without the firmware.
#include "host_stubs.h"
#else
// ---- OS 1.54 ----------------------------------------------------------------
#define AUTO_CHANNEL   (*(volatile int32_t *)0x4197c6fc)  // 0-15
#define ACTIVE_TRACK   (*(volatile int32_t *)0x4197c6b4)  // 0-7 audio, 8-15 MIDI A-H
#define TRACK_CHANNEL  ((volatile int32_t *)0x4197c700)   // 16 longs, as the router reads
#define RECEIVE_CC     (*(volatile int32_t *)0x4197c77c)  // PORT CONFIG > RECEIVE CC/NRPN
#define OUTPUT_TO      (*(volatile uint32_t *)0x421c9560) // bit 0 MIDI, bit 1 USB

// The DIN transmit ring's state, read as din_send reads it.
#define TX_PENDING     (*(volatile int32_t *)0x4060f7e8)
#define TX_BASE        (*(volatile uint32_t *)0x4060f7e0)
#define TX_SENT        (*(volatile uint32_t *)0x4060f7ec)
#define DMA37_A        (*(volatile uint16_t *)0xfc0454b4)
#define DMA37_B        (*(volatile uint16_t *)0xfc0454bc)
#define TX_RING        4096

typedef int32_t (*channel_fn)(int32_t midi_track);
typedef void (*din_send_fn)(int32_t len, const uint8_t *bytes);
typedef void (*handler_fn)(const uint8_t *msg, int32_t len, int32_t flag);

#define CHANNEL        ((channel_fn)0x400e09d8)   // a MIDI track's CHAN, -1 when off
#define DIN_SEND       ((din_send_fn)0x400028c6)
#define STOCK_CC       ((handler_fn)0x400d4cb8)
#endif

#define ID_PB          21
#define ID_AT          22
#define ID_MW          23

#define TAKE_OVER      ((int32_t)0x80000000)      // me_filter: stock must not send

// ---- settings (SETTINGS rows in rows.s; off at power-on, not saved) ---------
volatile uint8_t me_pb_on, me_at_on, me_mw_on;

// ---- state per output channel -----------------------------------------------
enum { PB, AT, MW, KINDS };
#define PRIORITY_BYTES 24       // about 8 ms of DIN line time

struct chan {
    int16_t track[KINDS];       // the track's own value: PB 0..16383, AT/MW 0..127
    int16_t live[KINDS];        // live input: PB offset -8192..8191, AT/MW 0..127
    uint8_t active[KINDS];      // live input has touched this channel
    uint8_t pending[KINDS];     // a value is due to go out
    uint8_t via_auto;           // touched from the auto channel: released on track change
};

static struct chan chans[16];
static volatile uint8_t flushing;
static volatile uint8_t waiting;
static volatile uint8_t initialised;
static int32_t last_active = -1;

// Counters for tests.
volatile uint32_t me_taken, me_sent, me_held, me_filtered, me_released;

#ifdef ME_HOST_TEST
static inline uint16_t irq_off(void) { return 0; }
static inline void irq_restore(uint16_t sr) { (void)sr; }
#else
static inline uint16_t irq_off(void)
{
    uint16_t sr;
    __asm__ volatile("move.w %%sr,%0\n\tmove.w #0x2700,%%sr" : "=d"(sr) :: "memory");
    return sr;
}

static inline void irq_restore(uint16_t sr)
{
    __asm__ volatile("move.w %0,%%sr" :: "d"(sr) : "memory");
}
#endif

static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void init(void)
{
    if (initialised)
        return;
    for (int ch = 0; ch < 16; ch++) {
        chans[ch].track[PB] = 8192;     // stock's centre until the track sends
        chans[ch].track[AT] = 0;
        chans[ch].track[MW] = 0;
        chans[ch].live[PB] = 0;
        chans[ch].live[AT] = 0;
        chans[ch].live[MW] = 0;
    }
    initialised = 1;
}

static uint8_t enabled(int kind)
{
    return kind == PB ? me_pb_on : kind == AT ? me_at_on : me_mw_on;
}

// Bytes waiting in the DIN transmit ring, as din_send computes them.
static uint32_t tx_queued(void)
{
    uint32_t used = ((uint32_t)DMA37_A - (uint32_t)DMA37_B + TX_BASE - TX_SENT) & (TX_RING - 1);
    return used + (uint32_t)TX_PENDING;
}

// The message for one channel and type, from the track's value and, while
// active, the live one. Called with interrupts masked.
static uint8_t compose(int ch, int kind, uint8_t *out)
{
    struct chan *c = &chans[ch];
    int32_t live = c->active[kind] ? c->live[kind] : 0;
    if (kind == PB) {
        int32_t v = clamp(c->track[PB] + live, 0, 16383);
        out[0] = (uint8_t)(0xE0 | ch);
        out[1] = (uint8_t)(v & 0x7F);
        out[2] = (uint8_t)(v >> 7);
        return 3;
    }
    if (kind == AT) {
        out[0] = (uint8_t)(0xD0 | ch);
        out[1] = (uint8_t)clamp(c->track[AT] + live, 0, 127);
        return 2;
    }
    out[0] = (uint8_t)(0xB0 | ch);
    out[1] = 1;
    out[2] = (uint8_t)clamp(c->track[MW] + live, 0, 127);
    return 3;
}

static void flush(void)
{
    uint16_t sr = irq_off();
    if (flushing) {                     // another task is sending: it will see ours
        waiting = 1;
        irq_restore(sr);
        return;
    }
    flushing = 1;
    waiting = 0;
    irq_restore(sr);

    for (int ch = 0; ch < 16; ch++) {
        for (int kind = 0; kind < KINDS; kind++) {
            uint8_t out[3];
            uint8_t len;
            sr = irq_off();
            if (!chans[ch].pending[kind]) {
                irq_restore(sr);
                continue;
            }
            if (!(OUTPUT_TO & 1)) {     // OUTPUT TO without MIDI: nothing to send
                chans[ch].pending[kind] = 0;
                irq_restore(sr);
                continue;
            }
            if (tx_queued() > PRIORITY_BYTES) {
                waiting = 1;            // the sequencer's bytes go first
                me_held++;
                irq_restore(sr);
                goto done;
            }
            len = compose(ch, kind, out);
            chans[ch].pending[kind] = 0;
            irq_restore(sr);
            DIN_SEND(len, out);
            me_sent++;
        }
    }
done:
    sr = irq_off();
    flushing = 0;
    irq_restore(sr);
}

// Give a channel back to its track: send the track's own value and stop
// adding to it. Called with interrupts masked.
static void release(int ch, int kind)
{
    struct chan *c = &chans[ch];
    if (!c->active[kind])
        return;
    c->active[kind] = 0;
    c->live[kind] = 0;
    c->pending[kind] = 1;
    me_released++;
}

static void release_auto(void)
{
    uint16_t sr = irq_off();
    for (int ch = 0; ch < 16; ch++) {
        if (!chans[ch].via_auto)
            continue;
        for (int kind = 0; kind < KINDS; kind++)
            release(ch, kind);
        chans[ch].via_auto = 0;
    }
    irq_restore(sr);
}

static void check_track(void)
{
    int32_t t = ACTIVE_TRACK;
    if (t != last_active) {
        if (last_active >= 0)
            release_auto();
        last_active = t;
    }
}

static void take(int kind, int32_t live, int32_t in_ch, int32_t flag)
{
    init();
    check_track();
    uint32_t tracks = 0;
    int from_auto = (flag & 0x14) != 0; // the auto channel (DIN 0x10, USB 4)
    if (from_auto) {
        int32_t t = ACTIVE_TRACK;
        if (t >= 8 && t < 16)
            tracks = 1u << (t - 8);
    } else {
        for (int t = 8; t < 16; t++)
            if (TRACK_CHANNEL[t] == in_ch)
                tracks |= 1u << (t - 8);
    }
    if (!tracks)
        return;
    me_taken++;
    for (int m = 0; m < 8; m++) {
        if (!(tracks & (1u << m)))
            continue;
        int32_t ch = CHANNEL(m);
        if (ch < 0 || ch > 15)
            continue;
        struct chan *c = &chans[ch];
        uint16_t sr = irq_off();
        c->live[kind] = (int16_t)live;
        c->active[kind] = 1;
        c->pending[kind] = 1;
        if (from_auto)
            c->via_auto = 1;
        irq_restore(sr);
    }
    flush();
}

// ---- the dispatcher's table entries ----------------------------------------
void me_pitch_bend(const uint8_t *msg, int32_t len, int32_t flag)
{
    if (me_pb_on && len == 3)
        take(PB, (int32_t)(msg[1] | (msg[2] << 7)) - 8192, msg[0] & 15, flag);
}

void me_aftertouch(const uint8_t *msg, int32_t len, int32_t flag)
{
    if (me_at_on && len == 2)
        take(AT, msg[1], msg[0] & 15, flag);
}

void me_control_change(const uint8_t *msg, int32_t len, int32_t flag)
{
    STOCK_CC(msg, len, flag);           // stock handling first, always
    if (me_mw_on && RECEIVE_CC && len == 3 && msg[1] == 1)
        take(MW, msg[2], msg[0] & 15, flag);
}

// ---- send(): the MIDI tracks' own PB, AT and MW -----------------------------
// Called from send.s at the entry of the stock send(track, channel, id,
// value, force), in the MIDI tracks' task. Returns the value for stock to
// send, or TAKE_OVER when this mod sends the combined value itself.
int32_t me_filter(int32_t track, int32_t channel, int32_t id, int32_t value, int32_t force)
{
    (void)track;
    (void)force;
    int kind;
    int32_t v;
    if (id == ID_PB) {
        kind = PB;
        v = clamp((int16_t)value, 0, 16383);
    } else if (id == ID_AT) {
        kind = AT;
        v = clamp((int16_t)value >> 8, 0, 127);
    } else if (id == ID_MW) {
        kind = MW;
        v = clamp((int16_t)value >> 8, 0, 127);
    } else {
        return value;
    }
    if (channel < 0 || channel > 15)
        return value;
    init();
    me_filtered++;
    struct chan *c = &chans[channel];
    uint16_t sr = irq_off();
    c->track[kind] = (int16_t)v;
    if (!c->active[kind]) {
        irq_restore(sr);
        return value;                   // no live input here: stock, untouched
    }
    c->pending[kind] = 1;
    irq_restore(sr);
    flush();
    return TAKE_OVER;
}

// ---- ev_tick: track changes, unticked rows, values that waited --------------
void me_tick(void *ctrl)
{
    (void)ctrl;
    if (!initialised)
        return;
    check_track();
    uint16_t sr = irq_off();
    for (int kind = 0; kind < KINDS; kind++) {
        if (enabled(kind))
            continue;
        for (int ch = 0; ch < 16; ch++)
            release(ch, kind);
    }
    irq_restore(sr);
    for (int ch = 0; ch < 16 && !waiting; ch++)
        for (int kind = 0; kind < KINDS; kind++)
            if (chans[ch].pending[kind])
                waiting = 1;
    if (waiting)
        flush();
}
