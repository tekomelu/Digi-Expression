// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI Expression, audio tracks: pitch bend, aftertouch and the mod wheel
// arriving at MIDI IN modulate an audio track's own sound, on the route the
// notes take (the auto channel to the active track, a TRACK CHANNEL to that
// track). Live playing only: nothing is recorded.
//
// The audio render keeps 53 parameter words per track (106 bytes from
// words + 18; 0..32512, the MIDI value in the top byte): slot 1-8 LFO1, 9-16
// LFO2, 17-24 the SRC page (17, knob A, is TUNE on the sample machines),
// 25-32 FLTR, 33-37 its second page, 38-45 AMP (as Digi Matrix measured).
// Stock's LFO stage adds its modulation to them each block. A 6-byte site
// at 0x40077eea, the next call after that stage (and after Digi Matrix's
// own hook), runs me_render on the words: it adds
//   TUNE   += bend offset x PB RANGE semitones, within TUNE's +-24
//   MW DEST += wheel x MW AMOUNT,   AT DEST += pressure x AT AMOUNT
// within 0..32512, in the same scale the stock LFO uses (+-64 at full value
// covers the parameter's whole range). It runs in the audio interrupt and
// only reads what the MIDI task wrote; nothing is stored in the words, so
// it stops the moment the live value is zero.

#include "expr.h"

#define PARAM_MAX      32512
#define SLOT_TUNE      17
#define SLOT_LAST      45
#define VOICE(p, v)    ((volatile int16_t *)((char *)(p) + 18 + 106 * (v)))

// TUNE: the parameter runs 40..88 around 64, one step a semitone (measured
// in digiemu: CC 16 at 88 and at 127 put the knob in the same place), so a
// word step of 256 is a semitone and the range is +-24 semitones.
#define TUNE_PER_SEMITONE 256
#define TUNE_MIN          (40 * 256)
#define TUNE_MAX          (88 * 256)

volatile int8_t me_bend_range = 2;
volatile int8_t me_mw_dest, me_at_dest;
volatile int8_t me_mw_amount = 32, me_at_amount = 32;

struct atrack {
    volatile int16_t live[KINDS];   // PB offset, AT, MW
    volatile uint8_t via_auto;
};

static struct atrack atracks[8];

void me_audio_take(int kind, int32_t live, uint32_t audio_tracks, int from_auto)
{
    for (int t = 0; t < 8; t++) {
        if (!(audio_tracks & (1u << t)))
            continue;
        atracks[t].live[kind] = (int16_t)live;
        if (from_auto)
            atracks[t].via_auto = 1;
    }
}

void me_audio_release_auto(void)
{
    for (int t = 0; t < 8; t++) {
        if (!atracks[t].via_auto)
            continue;
        for (int k = 0; k < KINDS; k++)
            atracks[t].live[k] = 0;
        atracks[t].via_auto = 0;
    }
}

void me_audio_release_kind(int kind)
{
    for (int t = 0; t < 8; t++)
        atracks[t].live[kind] = 0;
}

static void add_within(volatile int16_t *w, int32_t x, int32_t lo, int32_t hi)
{
    x += *w;
    if (x < lo)
        x = lo;
    else if (x > hi)
        x = hi;
    *w = (int16_t)x;
}

static void add(volatile int16_t *w, int32_t x)
{
    add_within(w, x, 0, PARAM_MAX);
}

// value 0..127 times amount -64..+64, the stock LFO's scale: full value at
// +-64 is the whole range (127 * 64 * 4 = 32512).
static int32_t scaled(int32_t value, int32_t amount)
{
    return value * amount * 4;
}

void me_render(void *words)
{
    int32_t range = me_bend_range;
    if (range > PB_RANGE_MAX)
        range = PB_RANGE_MAX;
    int32_t mw_dest = me_mw_dest, at_dest = me_at_dest;
    int32_t mw_amount = me_mw_amount, at_amount = me_at_amount;
    for (int t = 0; t < 8; t++) {
        struct atrack *a = &atracks[t];
        int32_t pb = a->live[PB], at = a->live[AT], mw = a->live[MW];
        if (!pb && !at && !mw)
            continue;
        volatile int16_t *v = VOICE(words, t);
        if (pb && range > 0)
            add_within(v + SLOT_TUNE, (pb * range * TUNE_PER_SEMITONE) / 8192, TUNE_MIN, TUNE_MAX);
        if (mw && mw_dest > 0 && mw_dest <= SLOT_LAST && mw_amount)
            add(v + mw_dest, scaled(mw, mw_amount));
        if (at && at_dest > 0 && at_dest <= SLOT_LAST && at_amount)
            add(v + at_dest, scaled(at, at_amount));
    }
}
