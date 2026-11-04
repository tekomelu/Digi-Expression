// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI Expression: what expr.c (MIDI tracks) and audio.c (audio tracks) share.
#ifndef MIDI_EXPRESSION_H
#define MIDI_EXPRESSION_H

#include <stdint.h>

enum { PB, AT, MW, KINDS };

// SETTINGS (rows.s, rows.c): off and the defaults at power-on, not saved.
extern volatile uint8_t me_pb_on, me_at_on, me_mw_on;
extern volatile int8_t me_bend_range;          // semitones, 0 = audio tracks not bent
#define PB_RANGE_MAX 12                         // an octave each way at most
extern volatile int8_t me_mw_dest, me_at_dest; // a sound slot (1..45), 0 = none
extern volatile int8_t me_mw_amount, me_at_amount; // -64..+64

// audio.c
// live: PB as an offset from centre (-8192..8191), AT and MW 0..127.
void me_audio_take(int kind, int32_t live, uint32_t audio_tracks, int from_auto);
void me_audio_release_auto(void);
void me_audio_release_kind(int kind);
void me_render(void *words);

#endif
