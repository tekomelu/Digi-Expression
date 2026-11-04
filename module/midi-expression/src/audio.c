// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI Expression, audio tracks: pitch bend, aftertouch, the mod wheel and the
// velocity of notes arriving at MIDI IN modulate an audio track's own sound,
// on the route the notes take (the auto channel to the active track, a TRACK
// CHANNEL to that track). Live playing only: nothing is recorded.
//
// The audio render keeps 53 parameter words per track (106 bytes from
// words + 18; 0..32512, the MIDI value in the top byte): slot 1-8 LFO1, 9-16
// LFO2, 17-24 the SRC page (17, knob A, is TUNE on the sample machines),
// 25-32 FLTR, 33-37 its second page, 38-45 AMP (as Digi Matrix measured).
// Each block (OS 1.54, render at 0x40077e9a..):
//   0x40074b9e builds the words, then
//   0x400ed766 the LFO stage reads the LFO slots and adds its modulation,
//   0x400ece8e (called at 0x40077eea) and what follows read every other slot.
// Measured in digiemu with scripts/probe-reads.py: slots 1-16 are read only
// inside the LFO stage, 17-36 and 38-45 only after it; 37 is never read.
// So two hooks add the live values:
//   me_render_lfo  at the LFO stage's entry (lfo.s): the LFO slots
//   me_render      at 0x40077eea (render.s): TUNE and every other slot
//   TUNE    += bend offset x BEND RANGE semitones, within TUNE's +-24
//   DEST    += source x AMOUNT, within 0..32512, in the stock LFO's scale
// with each track's own settings from the TRIG page's second page (page.c).
// Both run in the audio interrupt and only read what the MIDI task wrote;
// nothing is stored in the words, so it stops the moment the value is zero.

#include "expr.h"

#define PARAM_MAX      32512
#define SLOT_TUNE      17
#define SLOT_LFO_LAST  16
#define SLOT_LAST      45
#define VOICE(p, v)    ((volatile int16_t *)((char *)(p) + 18 + 106 * (v)))

// TUNE: the parameter runs 40..88 around 64, one step a semitone (measured
// in digiemu: CC 16 at 88 and at 127 put the knob in the same place), so a
// word step of 256 is a semitone and the range is +-24 semitones.
#define TUNE_PER_SEMITONE 256
#define TUNE_MIN          (40 * 256)
#define TUNE_MAX          (88 * 256)

struct atrack {
    volatile int16_t live[AKINDS];  // PB offset, AT, MW, the last note's velocity
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

void me_audio_velocity(int t, int32_t velocity, int from_auto)
{
    if (t < 0 || t > 7)
        return;
    atracks[t].live[VELK] = (int16_t)(velocity < 0 ? 0 : velocity > 127 ? 127 : velocity);
    if (from_auto)
        atracks[t].via_auto = 1;
}

void me_audio_release_auto(void)
{
    for (int t = 0; t < 8; t++) {
        if (!atracks[t].via_auto)
            continue;
        for (int k = 0; k < AKINDS; k++)
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

static int32_t held(int32_t amount)
{
    return amount > AMOUNT_MAX ? AMOUNT_MAX : amount < -AMOUNT_MAX ? -AMOUNT_MAX : amount;
}

// One route: source value 0..127 times amount -64.00..+64.00 (in 1/256) into
// slot `dest`, in the stock LFO's scale (full value at +-64 is the whole
// range: 127 * 64 * 256 / 64 = 32512). lfo: this pass does the LFO slots,
// else the rest.
static void route(volatile int16_t *v, int32_t dest, int32_t value, int32_t amount, int lfo)
{
    if (!value || dest <= 0 || dest > SLOT_LAST || !(amount = held(amount)))
        return;
    if ((dest <= SLOT_LFO_LAST) != lfo)
        return;
    int32_t x = value * amount / 64;
    if (dest == SLOT_TUNE)
        add_within(v + dest, x, TUNE_MIN, TUNE_MAX);
    else
        add_within(v + dest, x, 0, PARAM_MAX);
}

static void render(void *words, int lfo)
{
    for (int t = 0; t < 8; t++) {
        struct atrack *a = &atracks[t];
        int32_t pb = a->live[PB], at = a->live[AT], mw = a->live[MW], vel = a->live[VELK];
        if (!pb && !at && !mw && !vel)
            continue;
        struct me_audio_cfg *cfg = &me_audio_cfg[t];
        volatile int16_t *v = VOICE(words, t);
        if (!lfo && pb) {
            int32_t range = cfg->range;
            if (range > PB_RANGE_MAX)
                range = PB_RANGE_MAX;
            if (range > 0)
                add_within(v + SLOT_TUNE, (pb * range * TUNE_PER_SEMITONE) / 8192, TUNE_MIN, TUNE_MAX);
        }
        route(v, cfg->mw_dest, mw, cfg->mw_amount, lfo);
        route(v, cfg->at_dest, at, cfg->at_amount, lfo);
        route(v, cfg->vel_dest, vel, cfg->vel_amount, lfo);
    }
}

void me_render(void *words)
{
    render(words, 0);
}

void me_render_lfo(void *words)
{
    render(words, 1);
}

// ---- velocity ------------------------------------------------------------------
// The note router (0x400d5c1c) hands each routed track the note with
// live_note(track, note, velocity, flag, 0, -1, -1) at 0x400d5d8e; vel.s
// calls this first and passes on what it returns. That one call is both what
// sounds and what the sequencer records. VEL SENS (0..100 %) scales how much
// the played velocity counts: 100 % as played, 50 % maps 0..127 to 64..127,
// 0 % plays every note at 127. An audio track also keeps the played velocity
// as a modulation source (VEL DEST).
int32_t me_note_velocity(int32_t track, int32_t velocity, int32_t flag)
{
    int32_t sens;
    if (track < 0 || track > 15 || velocity <= 0 || velocity > 127)
        return velocity;
    if (track < 8) {
        me_audio_velocity(track, velocity, (flag & 0x14) != 0);
        sens = me_audio_cfg[track].vel_sens;
    } else {
        sens = me_midi_cfg[track - 8].vel_sens;
    }
    if (sens >= VEL_SENS_MAX)
        return velocity;
    if (sens < 0)
        sens = 0;
    int32_t v = velocity + ((127 - velocity) * (VEL_SENS_MAX - sens) + VEL_SENS_MAX / 2) / VEL_SENS_MAX;
    return v < 1 ? 1 : v > 127 ? 127 : v;
}
