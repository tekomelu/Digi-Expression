// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI Expression for the Digitakt mk1, OS 1.54: pitch bend, channel
// aftertouch and the mod wheel arriving at MIDI IN go out to the synth a MIDI
// track plays, following the same route as the notes.
//
// Stock, the MIDI task hands each incoming message to one of sixteen
// handlers, chosen by its type, from a table at 0x401b8dd4: message, length
// and a flag (0x10 on the auto channel, 8 otherwise; 4 and 2 from USB). The
// entries for channel aftertouch (0xD) and pitch bend (0xE) are an empty
// function, and the control-change handler (0xB) does not forward CC 1. This
// mod points the D and E entries at its own handlers and wraps the B entry
// (its handler runs the stock one first, always). See docs/RESEARCH-OS154.md
// in the project repository for how these addresses were found.
//
// Routing is the stock note router's (0x400d5c1c): on the auto channel, the
// active track only; otherwise every track whose TRACK CHANNEL matches. Of
// those, MIDI tracks A-H with CHAN on get the message, re-channelled to their
// CHAN.
//
// Sending: each output channel has one slot per message type, and a newer
// value replaces one not yet sent. Slots go out through the stock DIN driver
// (din_send, which masks interrupts while it copies into its 4 KB transmit
// ring) only while that ring holds at most PRIORITY_BYTES, so expression
// never queues in front of a burst of sequencer notes. What waits is sent by
// the next message or the UI's 30 Hz tick. DIN only; USB is not sent to.

#include <stdint.h>

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

// ---- settings (SETTINGS rows in rows.s; off at power-on, not saved) ---------
volatile uint8_t me_pb_on, me_at_on, me_mw_on;

// ---- slots ------------------------------------------------------------------
enum { PB, AT, MW, KINDS };
#define PRIORITY_BYTES 24       // about 8 ms of DIN line time

struct slot {
    uint8_t pending;
    uint8_t len;
    uint8_t msg[3];
};

static struct slot slots[16][KINDS];
static volatile uint8_t flushing;
static volatile uint8_t waiting;

// Counters for tests: messages taken, sent, and replaced before they went.
volatile uint32_t me_taken, me_sent, me_replaced, me_held;

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

// Bytes waiting in the DIN transmit ring, as din_send computes them.
static uint32_t tx_queued(void)
{
    uint32_t used = ((uint32_t)DMA37_A - (uint32_t)DMA37_B + TX_BASE - TX_SENT) & (TX_RING - 1);
    return used + (uint32_t)TX_PENDING;
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
            struct slot *s = &slots[ch][kind];
            uint8_t out[3];
            uint8_t len;
            sr = irq_off();
            if (!s->pending) {
                irq_restore(sr);
                continue;
            }
            if (!(OUTPUT_TO & 1)) {     // OUTPUT TO without MIDI: nothing to send
                s->pending = 0;
                irq_restore(sr);
                continue;
            }
            if (tx_queued() > PRIORITY_BYTES) {
                waiting = 1;            // the sequencer's bytes go first
                me_held++;
                irq_restore(sr);
                goto done;
            }
            len = s->len;
            out[0] = s->msg[0];
            out[1] = s->msg[1];
            out[2] = s->msg[2];
            s->pending = 0;
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

static void take(int kind, const uint8_t *msg, int32_t len, int32_t flag)
{
    int32_t in_ch = msg[0] & 15;
    uint32_t tracks = 0;
    if (flag & 0x14) {                  // the auto channel (DIN 0x10, USB 4)
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
        struct slot *s = &slots[ch][kind];
        uint16_t sr = irq_off();
        if (s->pending)
            me_replaced++;
        s->msg[0] = (uint8_t)((msg[0] & 0xF0) | ch);
        s->msg[1] = msg[1];
        s->msg[2] = len > 2 ? msg[2] : 0;
        s->len = (uint8_t)len;
        s->pending = 1;
        irq_restore(sr);
    }
    flush();
}

// ---- the dispatcher's table entries ----------------------------------------
void me_pitch_bend(const uint8_t *msg, int32_t len, int32_t flag)
{
    if (me_pb_on && len == 3)
        take(PB, msg, 3, flag);
}

void me_aftertouch(const uint8_t *msg, int32_t len, int32_t flag)
{
    if (me_at_on && len == 2)
        take(AT, msg, 2, flag);
}

void me_control_change(const uint8_t *msg, int32_t len, int32_t flag)
{
    STOCK_CC(msg, len, flag);           // stock handling first, always
    if (me_mw_on && RECEIVE_CC && len == 3 && msg[1] == 1)
        take(MW, msg, 3, flag);
}

// ---- ev_tick: send what waited for the ring --------------------------------
void me_tick(void *ctrl)
{
    (void)ctrl;
    if (waiting)
        flush();
}
