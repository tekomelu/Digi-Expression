// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI Expression: what expr.c (MIDI tracks), audio.c (audio tracks) and
// page.c (the TRIG page's second page) share.
#ifndef MIDI_EXPRESSION_H
#define MIDI_EXPRESSION_H

#include <stdint.h>

enum { PB, AT, MW, KINDS };
// What a MIDI track sends: the three above, summed with the track's own
// values, or the wheel and the pressure as a plain CC of their own.
enum { XMW = KINDS, XAT, OUTS };

// SETTINGS (rows.s): the THRU rows, unticked at power-on, not saved.
extern volatile uint8_t me_pb_on, me_at_on, me_mw_on;

// Per track, on the TRIG page's second page (page.c). Defaults at power-on;
// held in RAM only, not saved with the project.
#define PB_RANGE_MAX 12                         // an octave each way at most
#define AMOUNT_MAX   64
#define CC_MAX       119                        // CC 120-127 are channel mode messages

struct me_audio_cfg {
    volatile int8_t range;                      // semitones, 0 = not bent
    volatile int8_t mw_dest, at_dest, vel_dest; // a sound slot (1..45), 0 = none
    volatile int8_t mw_amount, at_amount, vel_amount;   // -64..+64
    volatile int8_t vel_sens;                   // 0..100 %: how much velocity counts
};

struct me_midi_cfg {
    volatile uint8_t pb_on, at_on, mw_on;       // this track passes them
    volatile uint8_t mw_cc;                     // 1: the track's MW (CC 1); else this CC
    volatile uint8_t at_cc;                     // 0: channel aftertouch; else this CC
    volatile int8_t vel_sens;                   // 0..100 %
};

#define VEL_SENS_MAX 100
#define VELK KINDS                              // audio tracks: velocity as a fourth source
#define AKINDS (KINDS + 1)

extern struct me_audio_cfg me_audio_cfg[8];
extern struct me_midi_cfg me_midi_cfg[8];

// audio.c
// live: PB as an offset from centre (-8192..8191), AT and MW 0..127.
void me_audio_take(int kind, int32_t live, uint32_t audio_tracks, int from_auto);
void me_audio_release_auto(void);
void me_audio_release_kind(int kind);
void me_audio_velocity(int t, int32_t velocity, int from_auto);
void me_render(void *words);                    // after the LFO stage: every slot but the LFOs'
void me_render_lfo(void *words);                // the LFO stage's entry: the LFO slots (1..16)

// vel.s / audio.c: a note from MIDI IN, on its way to a track (0..15).
int32_t me_note_velocity(int32_t track, int32_t velocity, int32_t flag);

// expr.c: a MIDI track's settings changed on the page: give its channel back.
void me_midi_release_track(int m);

#endif
