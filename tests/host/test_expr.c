// Host unit tests for MIDI Expression's logic (module/midi-expression/src/expr.c).
//   cc -std=c11 -Wall -DME_HOST_TEST -I tests/host tests/host/test_expr.c -o build/test_expr && build/test_expr
#include <stdio.h>
#include <string.h>

#include "../../module/midi-expression/src/expr.c"
static int16_t h_os_vel[8];
#define OS_VEL h_os_vel
#include "../../module/midi-expression/src/audio.c"
#include "../../module/midi-expression/src/page.c"
#include <stdarg.h>

volatile int32_t h_auto_channel = 13, h_active_track = 8, h_track_channel[16], h_receive_cc = 1;
volatile uint32_t h_output_to = 3;
volatile int32_t h_tx_pending;
volatile uint32_t h_tx_base, h_tx_sent;
volatile uint16_t h_dma_a, h_dma_b;
volatile int8_t h_params[8 * 106];

int h_trig_page, h_func_held, h_push_held, h_draw_calls, h_knob_calls;
static char drawn[2048];
void h_text(void *bmp, const void *font, int x, int y, int colour, const char *fmt, ...)
{
    (void)bmp; (void)font; (void)x; (void)y; (void)colour;
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (strlen(drawn) + strlen(line) + 2 < sizeof drawn) { strcat(drawn, line); strcat(drawn, "|"); }
    h_draw_calls++;
}
void h_fillrect(void *bmp, int x0, int y0, int x1, int y1, int colour) { (void)bmp; (void)x0; (void)y0; (void)x1; (void)y1; (void)colour; }

#define ALL(field, v) do { for (int t_ = 0; t_ < 8; t_++) me_audio_cfg[t_].field = (v); } while (0)

static int32_t chan_of[8];              // MIDI track A-H -> CHAN (0-15), -1 off
static uint8_t sent[256][3];
static int sent_len[256], nsent, stock_cc_calls;

int32_t h_channel(int32_t m) { return chan_of[m]; }
void h_din_send(int32_t len, const uint8_t *b)
{
    memcpy(sent[nsent], b, (size_t)len);
    sent_len[nsent++] = len;
}
void h_stock_cc(const uint8_t *msg, int32_t len, int32_t flag) { (void)msg; (void)len; (void)flag; stock_cc_calls++; }

static uint32_t rnd_state = 12345;
static int rnd_next(int n) { rnd_state = rnd_state * 1103515245u + 12345u; return (int)((rnd_state >> 16) % (uint32_t)n); }

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void reset(void)
{
    memset((void *)chans, 0, sizeof chans);
    initialised = 0; flushing = 0; waiting = 0; last_active = -1;
    clock_stamp = 0;
    memset((void *)h_params, 0, sizeof h_params);
    for (int t = 0; t < 16; t++) h_track_channel[t] = t;   // tracks 1-8 ch 1-8, A-H ch 9-16
    for (int m = 0; m < 8; m++) chan_of[m] = -1;
    chan_of[0] = 0;                                        // MIDI track A: CHAN on, channel 1
    h_active_track = 8; h_auto_channel = 13;
    h_tx_pending = 0; h_tx_base = h_tx_sent = 0; h_dma_a = h_dma_b = 0;
    nsent = 0; stock_cc_calls = 0;
    memset((void *)atracks, 0, sizeof atracks);
    ALL(range, 2); ALL(mw_dest, 0); ALL(at_dest, 0); ALL(vel_dest, 0); ALL(mw_amount, 0); ALL(at_amount, 0); ALL(vel_amount, 0); ALL(vel_sens, 100);
    for (int m = 0; m < 8; m++) {
        me_midi_cfg[m].thru = 1; me_midi_cfg[m].vel_sens = 100;
        me_midi_cfg[m].mw_dest = me_midi_cfg[m].at_dest = me_midi_cfg[m].vel_dest = 0;
        me_midi_cfg[m].mw_amount = me_midi_cfg[m].at_amount = me_midi_cfg[m].vel_amount = 0;
    }
    me_page_open = 0; recompose = 0; pend = 0; hold = 0; list_cell = -1; list_t = 0; acc_track = -1;
    for (int k = 0; k < CELLS; k++) { acc[k] = 0; shown[k] = 0; idle_counts[k] = 0; snap_rest[k] = 0; }
    h_trig_page = 0; h_func_held = 0;
}

// A render block's parameter words: 18 bytes, then 8 tracks x 53 slots.
static int16_t words[9 + 53 * 8 + 8];
static int16_t *slot(int track, int s) { return (int16_t *)((char *)words + 18 + 106 * track) + s; }
static void words_reset(int16_t value)
{
    for (int t = 0; t < 8; t++)
        for (int s = 0; s < 53; s++)
            *slot(t, s) = value;
}

#define ID_PB 21
#define ID_AT 22
#define ID_MW 23
static int pb_of(int i) { return sent[i][1] | (sent[i][2] << 7); }

static void pb_in(int ch, int value, int flag) { uint8_t m[3] = { (uint8_t)(0xE0 | ch), (uint8_t)(value & 127), (uint8_t)(value >> 7) }; me_pitch_bend(m, 3, flag); }
static void at_in(int ch, int v, int flag) { uint8_t m[2] = { (uint8_t)(0xD0 | ch), (uint8_t)v }; me_aftertouch(m, 2, flag); }
static void mw_in(int ch, int v, int flag) { uint8_t m[3] = { (uint8_t)(0xB0 | ch), 1, (uint8_t)v }; me_control_change(m, 3, flag); }
// What the MIDI tracks' task would hand stock send(): PB 14-bit, AT/MW 8.8 fixed.
static int32_t track_pb(int ch, int v) { return me_filter(0, ch, ID_PB, v, 0); }
static int32_t track_at(int ch, int v) { return me_filter(0, ch, ID_AT, v << 8, 0); }
static int32_t track_mw(int ch, int v) { return me_filter(0, ch, ID_MW, v << 8, 0); }

static void bc_in(int ch, int v, int flag) { uint8_t m[3] = { (uint8_t)(0xB0 | ch), 2, (uint8_t)v }; me_control_change(m, 3, flag); }
static int32_t track_id(int ch, int id, int v) { return me_filter(0, ch, id, id == ID_PB ? v : v << 8, 0); }
#define SEL(m, k, cc) (h_params[106 * (m) + 2 * (33 + (k))] = (int8_t)(cc))
static int last_cc(int cc) { for (int i = nsent - 1; i >= 0; i--) if ((sent[i][0] & 0xF0) == 0xB0 && sent[i][1] == cc) return sent[i][2]; return -1; }
static int count_cc(int from, int cc) { int n = 0; for (int i = from; i < nsent; i++) if ((sent[i][0] & 0xF0) == 0xB0 && sent[i][1] == cc) n++; return n; }

#define AUTO 0x10
#define TRACK 8

int main(void)
{
    // 1. No live input: the track's sends go out untouched, stock sends them.
    reset();
    CHECK(track_pb(0, 9000) == 9000, "untouched PB passes");
    CHECK(track_at(0, 40) == (40 << 8), "untouched AT passes");
    CHECK(track_mw(0, 20) == (20 << 8), "untouched MW passes");
    CHECK(nsent == 0, "nothing sent by the mod: %d", nsent);

    // 2. Live bend with the track at centre: out exactly as played.
    reset();
    pb_in(13, 12288, AUTO);
    CHECK(nsent == 1 && sent[0][0] == 0xE0 && pb_of(0) == 12288, "bend out as played: %02x %d", sent[0][0], pb_of(0));

    // 3. LFO/lock on the track plus live bend add up (+1 and +1 semitone at a 2-semitone range = 4096 each).
    reset();
    track_pb(0, 8192 + 2048);                     // the track's LFO at +1 semitone (range +/-2: 4096 a semitone? use offsets)
    pb_in(13, 8192 + 2048, AUTO);                 // live +2048
    CHECK(pb_of(nsent - 1) == 8192 + 4096, "track + live: %d", pb_of(nsent - 1));
    int32_t r = track_pb(0, 8192 - 2048);         // the LFO swings to -2048 while the bend is held
    CHECK(r == TAKE_OVER, "the track's send is taken over");
    CHECK(sent[nsent - 1][0] == 0xE0 && pb_of(nsent - 1) == 8192, "LFO -2048 + live +2048 = centre: %d", pb_of(nsent - 1));

    // 4. Capped to the MIDI limits.
    reset();
    track_pb(0, 16000);
    pb_in(13, 16383, AUTO);
    CHECK(pb_of(nsent - 1) == 16383, "PB capped at 16383: %d", pb_of(nsent - 1));
    pb_in(13, 0, AUTO);
    track_pb(0, 100);
    CHECK(pb_of(nsent - 1) == 0, "PB capped at 0: %d", pb_of(nsent - 1));
    reset();
    track_at(0, 100);
    at_in(13, 100, AUTO);
    CHECK(sent[nsent - 1][0] == 0xD0 && sent[nsent - 1][1] == 127, "AT capped at 127: %d", sent[nsent - 1][1]);
    track_mw(0, 90);
    mw_in(13, 90, AUTO);
    CHECK(sent[nsent - 1][0] == 0xB0 && sent[nsent - 1][1] == 1 && sent[nsent - 1][2] == 127, "MW capped at 127: %d", sent[nsent - 1][2]);
    CHECK(stock_cc_calls == 1, "stock CC handler ran: %d", stock_cc_calls);
    for (int i = 0; i < nsent; i++)
        for (int j = 1; j < sent_len[i]; j++)
            CHECK(sent[i][j] <= 127, "data byte %d of message %d is %d", j, i, sent[i][j]);

    // 5. AT and MW add to the track's own value.
    reset();
    track_at(0, 30);
    at_in(13, 40, AUTO);
    CHECK(sent[nsent - 1][1] == 70, "AT 30 + 40: %d", sent[nsent - 1][1]);
    track_mw(0, 50);
    mw_in(13, 10, AUTO);
    CHECK(sent[nsent - 1][2] == 60, "MW 50 + 10: %d", sent[nsent - 1][2]);

    // 6. Leaving the track restores its own values, then the new track only on the next move.
    reset();
    track_pb(0, 8192 + 1000);
    track_at(0, 20);
    track_mw(0, 33);
    pb_in(13, 12000, AUTO);
    at_in(13, 90, AUTO);
    mw_in(13, 64, AUTO);
    int before = nsent;
    h_active_track = 0;                           // audio track 1 selected
    me_tick(0);
    int got_pb = -1, got_at = -1, got_mw = -1;
    for (int i = before; i < nsent; i++) {
        if (sent[i][0] == 0xE0) got_pb = pb_of(i);
        if (sent[i][0] == 0xD0) got_at = sent[i][1];
        if (sent[i][0] == 0xB0) got_mw = sent[i][2];
    }
    CHECK(got_pb == 8192 + 1000 && got_at == 20 && got_mw == 33, "restored to the track's own: PB %d AT %d MW %d", got_pb, got_at, got_mw);
    CHECK(track_pb(0, 8192 + 500) == 8192 + 500, "released: the track's sends pass again");
    before = nsent;
    pb_in(13, 15000, AUTO);                       // audio track selected: the auto channel reaches no MIDI track
    CHECK(nsent == before, "audio track selected: nothing sent");
    chan_of[1] = 3;                               // MIDI track B, CHAN 4
    h_active_track = 9;
    me_tick(0);
    CHECK(nsent == before, "selecting B sends nothing until the next move: %d", nsent - before);
    mw_in(13, 10, AUTO);
    CHECK(nsent == before + 1 && sent[nsent - 1][0] == 0xB3 && sent[nsent - 1][2] == 10, "B gets the wheel on the next move");

    // 7. Track-channel input is not released on a track change.
    reset();
    pb_in(8, 12000, TRACK);                       // track A's own channel 9
    before = nsent;
    h_active_track = 0;
    me_tick(0);
    CHECK(nsent == before, "track-channel bend kept on track change: %d", nsent - before);

    // 8. THRU off on the track gives the synth the track's value back, and passes nothing more.
    reset();
    track_pb(0, 8192);
    pb_in(13, 14000, AUTO);
    me_midi_cfg[0].thru = 0;
    before = nsent;
    me_midi_changed(0);
    CHECK(nsent == before + 1 && pb_of(nsent - 1) == 8192, "THRU off: PB back to the track's: %d", nsent > before ? pb_of(nsent - 1) : -1);
    before = nsent;
    pb_in(13, 2000, AUTO);
    CHECK(nsent == before, "THRU off: a bend sends nothing");
    CHECK(track_pb(0, 9000) == 9000, "and the track's own bend is stock again");

    // 9. Priority: nothing goes out while the DIN ring is busy; the newest value goes once it drains.
    reset();
    h_tx_pending = 100;
    pb_in(13, 9000, AUTO);
    pb_in(13, 10000, AUTO);
    CHECK(nsent == 0, "held while busy: %d", nsent);
    h_tx_pending = 0;
    me_tick(0);
    CHECK(nsent == 1 && pb_of(0) == 10000, "newest value after: %d sent, %d", nsent, nsent ? pb_of(0) : -1);

    // 10. Routing: CHAN off, audio track selected, unmatched channel.
    reset();
    chan_of[0] = -1;
    pb_in(13, 9000, AUTO);
    CHECK(nsent == 0, "CHAN off: nothing");
    reset();
    h_active_track = 2;
    at_in(13, 50, AUTO);
    CHECK(nsent == 0, "audio track active: nothing");
    pb_in(8, 9000, TRACK);
    CHECK(nsent == 1, "track A's channel still reaches A");
    reset();
    pb_in(4, 9000, TRACK);
    CHECK(nsent == 0, "channel 5 (audio track 5 only): nothing");

    // 11. OUTPUT TO without MIDI: nothing.
    reset();
    h_output_to = 2;
    pb_in(13, 9000, AUTO);
    CHECK(nsent == 0, "OUTPUT TO USB only: nothing");
    h_output_to = 3;

    // 12. Other ids pass straight through.
    reset();
    pb_in(13, 9000, AUTO);
    CHECK(me_filter(0, 0, 24, 77 << 8, 0) == (77 << 8), "breath (id 24) untouched while only PB is live");
    CHECK(me_filter(0, 0, 25, 5 << 8, 1) == (5 << 8), "CC VAL (id 25) untouched");

    // ---- audio tracks -------------------------------------------------------
    // 13. Pitch bend on an audio track: TUNE is 64 x 256 at 0, 256 a semitone, 40..88 (+-24).
    reset();
    h_active_track = 0;                           // audio track 1
    words_reset(16384);
    pb_in(13, 8192 + 4096, AUTO);                 // half up at PB RANGE 2
    me_render(words);
    CHECK(*slot(0, 17) == 16384 + 256, "TUNE +1 semitone: %d", *slot(0, 17) - 16384);
    CHECK(*slot(1, 17) == 16384, "other tracks untouched");
    CHECK(nsent == 0, "no MIDI out for an audio track: %d", nsent);
    words_reset(16384);
    pb_in(13, 0, AUTO);                           // full down
    me_render(words);
    CHECK(*slot(0, 17) == 16384 - 512, "TUNE -2 semitones: %d", *slot(0, 17) - 16384);
    ALL(range, 12);
    words_reset(16384);
    pb_in(13, 16383, AUTO);                       // full up: 8191/8192 of 12 semitones
    me_render(words);
    CHECK(*slot(0, 17) == 16384 + 3071, "TUNE +12: %d", *slot(0, 17) - 16384);
    words_reset(16384);
    pb_in(13, 0, AUTO);
    me_render(words);
    CHECK(*slot(0, 17) == 16384 - 3072, "TUNE -12: %d", *slot(0, 17) - 16384);
    ALL(range, 24);                           // more than the row allows: held to 12
    words_reset(16384);
    pb_in(13, 0, AUTO);
    me_render(words);
    CHECK(*slot(0, 17) == 16384 - 3072, "range held to 12: %d", *slot(0, 17) - 16384);
    ALL(range, 12);
    words_reset(16384 + 13 * 256);                /* TUNE knob at +13, bend +12: TUNE's own top */
    pb_in(13, 16383, AUTO);
    me_render(words);
    CHECK(*slot(0, 17) == TUNE_MAX, "capped at TUNE +24: %d", *slot(0, 17));
    words_reset(16384 - 13 * 256);
    pb_in(13, 0, AUTO);
    me_render(words);
    CHECK(*slot(0, 17) == TUNE_MIN, "capped at TUNE -24: %d", *slot(0, 17));
    ALL(range, 0);
    words_reset(16384);
    me_render(words);
    CHECK(*slot(0, 17) == 16384, "PB RANGE OFF: TUNE untouched");

    // 14. Mod wheel and aftertouch routes add in the stock LFO's scale, within 0..32512.
    reset();
    h_active_track = 2;                           // audio track 3
    ALL(mw_dest, 26); ALL(mw_amount, 64 * 256);           // FLT.FREQ, full
    ALL(at_dest, 44); ALL(at_amount, -32 * 256);          // AMP.PAN, half, down
    words_reset(20000);
    mw_in(13, 127, AUTO);
    at_in(13, 100, AUTO);
    me_render(words);
    CHECK(*slot(2, 26) == PARAM_MAX, "MW full to FLT.FREQ: capped at the top: %d", *slot(2, 26));
    CHECK(*slot(2, 44) == 20000 - 100 * 32 * 4, "AT 100 x -32 on AMP.PAN: %d", *slot(2, 44));
    words_reset(1000);
    me_render(words);
    CHECK(*slot(2, 44) == 0, "capped at 0: %d", *slot(2, 44));
    words_reset(10000);
    mw_in(13, 64, AUTO);
    ALL(mw_amount, 32 * 256);
    me_render(words);
    CHECK(*slot(2, 26) == 10000 + 64 * 32 * 4, "MW 64 x +32: %d", *slot(2, 26) - 10000);
    for (int t = 0; t < 8; t++)
        for (int s = 0; s < 53; s++)
            CHECK(*slot(t, s) >= 0 && *slot(t, s) <= PARAM_MAX, "word %d/%d in range: %d", t, s, *slot(t, s));

    // 15. Changing track stops the old track's modulation at once.
    words_reset(10000);
    h_active_track = 3;
    me_tick(0);
    me_render(words);
    CHECK(*slot(2, 26) == 10000 && *slot(2, 44) == 10000, "released on track change: %d %d", *slot(2, 26), *slot(2, 44));
    CHECK(*slot(3, 26) == 10000, "the new track only follows the next move");
    mw_in(13, 32, AUTO);
    me_render(words);
    CHECK(*slot(3, 26) == 10000 + 32 * 32 * 4, "new track after a move: %d", *slot(3, 26) - 10000);

    // 16. A TRACK CHANNEL reaches its audio track whatever is selected, and is kept on a track change.
    reset();
    ALL(mw_dest, 27); ALL(mw_amount, 32 * 256);    // FLT.RESO
    words_reset(5000);
    mw_in(4, 100, TRACK);                         // channel 5: audio track 5
    h_active_track = 1;
    me_tick(0);
    me_render(words);
    CHECK(*slot(4, 27) == 5000 + 100 * 32 * 4, "track-channel route kept: %d", *slot(4, 27) - 5000);



    // ---- per track (the TRIG page's second page) -----------------------------
    // 18. Audio settings are each track's own.
    reset();
    me_audio_cfg[0].range = 12;
    me_audio_cfg[1].range = 0;
    h_track_channel[1] = 0;                       // tracks 1 and 2 both on channel 1
    words_reset(16384);
    pb_in(0, 0, TRACK);                           // full down
    me_render(words);
    CHECK(*slot(0, 17) == 16384 - 3072, "track 1 at 12: %d", *slot(0, 17) - 16384);
    CHECK(*slot(1, 17) == 16384, "track 2 at OFF: %d", *slot(1, 17) - 16384);
    me_audio_cfg[0].mw_dest = 26; me_audio_cfg[0].mw_amount = 10 * 256;
    me_audio_cfg[1].mw_dest = 27; me_audio_cfg[1].mw_amount = -10 * 256;
    words_reset(10000);
    mw_in(0, 100, TRACK);
    me_render(words);
    CHECK(*slot(0, 26) == 10000 + 100 * 10 * 4 && *slot(0, 27) == 10000, "track 1 wheel to FLT.FREQ: %d %d", *slot(0, 26), *slot(0, 27));
    CHECK(*slot(1, 27) == 10000 - 100 * 10 * 4 && *slot(1, 26) == 10000, "track 2 wheel to FLT.RESO: %d %d", *slot(1, 27), *slot(1, 26));

    // 22. V.SNS on what plays: an audio voice's stored velocity (8.8), a MIDI track's note out.
    reset();
    h_os_vel[0] = 40 << 8;
    me_voice_velocity(0);
    CHECK(h_os_vel[0] == 40 << 8, "100 %%: as played: %d", h_os_vel[0]);
    me_audio_cfg[0].vel_sens = 50;
    h_os_vel[0] = 1 << 8;
    me_voice_velocity(0);
    CHECK(h_os_vel[0] == (256 + ((32512 - 256) * 50 + 50) / 100), "50 %%: 1 -> about 64: %d", h_os_vel[0] >> 8);
    h_os_vel[0] = 32512;
    me_voice_velocity(0);
    CHECK(h_os_vel[0] == 32512, "50 %%: full stays full");
    me_audio_cfg[0].vel_sens = 0;
    h_os_vel[0] = 10 << 8;
    me_voice_velocity(0);
    CHECK(h_os_vel[0] == 32512, "0 %%: every note full");
    h_os_vel[3] = 0;
    me_voice_velocity(3);
    CHECK(h_os_vel[3] == 0, "no velocity: untouched");
    for (int v = 1; v < 128; v++)
        for (int sns = 0; sns <= 100; sns += 10) {
            int r2 = me_velocity_scaled(v, sns);
            CHECK(r2 >= v && r2 <= 127, "sens %d v %d -> %d", sns, v, r2);
        }
    CHECK(me_velocity_scaled(1, 50) == 64 && me_velocity_scaled(64, 50) == 96, "MIDI scale: 1 -> 64, 64 -> 96 at 50 %%");
    // MIDI tracks: notes out through notes_append
    chan_of[2] = 4;
    me_midi_cfg[2].vel_sens = 50;
    {
        const uint8_t on[3] = { 0x94, 60, 1 }, off[3] = { 0x84, 60, 0 }, other[3] = { 0x95, 60, 1 };
        const uint8_t *r = me_note_out(3, on);
        CHECK(r != on && r[0] == 0x94 && r[1] == 60 && r[2] == 64, "MIDI C's note on its channel: velocity 1 -> %d", r[2]);
        CHECK(on[2] == 1, "the caller's bytes untouched");
        CHECK(me_note_out(3, off) == off, "a note off passes as it is");
        CHECK(me_note_out(3, other) == other, "another channel passes as it is");
        const uint8_t two[6] = { 0x94, 60, 100, 0x94, 64, 2 };
        r = me_note_out(6, two);
        CHECK(r[2] == 114 && r[5] == 65, "two notes in one call: %d %d", r[2], r[5]);
        me_midi_cfg[2].vel_sens = 100;
        CHECK(me_note_out(3, on) == on, "100 %%: as played");
    }

    // 23. Velocity as a source: the last note the track played (live or from the sequencer),
    //     unscaled, x V.DEP into VEL DEST; it stays on a track change.
    reset();
    me_tick(0);
    h_active_track = 0;
    me_audio_cfg[0].vel_dest = 26; me_audio_cfg[0].vel_amount = 16 * 256;
    h_os_vel[0] = 100 << 8;
    me_voice_velocity(0);
    words_reset(10000);
    me_render(words);
    CHECK(*slot(0, 26) == 10000 + 100 * 16 * 4, "velocity 100 x 16 on FLT.FREQ: %d", *slot(0, 26) - 10000);
    me_audio_cfg[0].vel_sens = 0;                 // the source is the played velocity, not the scaled one
    h_os_vel[0] = 20 << 8;
    me_voice_velocity(0);
    words_reset(10000);
    me_render(words);
    CHECK(*slot(0, 26) == 10000 + 20 * 16 * 4, "the next note, unscaled: %d", *slot(0, 26) - 10000);
    CHECK(h_os_vel[0] == 32512, "while it plays at full (V.SNS 0)");
    h_active_track = 1;
    me_tick(0);
    words_reset(10000);
    me_render(words);
    CHECK(*slot(0, 26) == 10000 + 20 * 16 * 4, "kept on a track change (the sequencer keeps playing it)");

    // 24. LFO destinations go in at the LFO stage's entry, the rest after it.
    reset();
    h_active_track = 0;
    me_audio_cfg[0].mw_dest = 8; me_audio_cfg[0].mw_amount = 32 * 256;      // L1.DEP
    me_audio_cfg[0].at_dest = 26; me_audio_cfg[0].at_amount = 32 * 256;     // FLT.FREQ
    mw_in(13, 64, AUTO);
    at_in(13, 64, AUTO);
    words_reset(10000);
    me_render(words);
    CHECK(*slot(0, 8) == 10000 && *slot(0, 26) == 10000 + 64 * 32 * 4, "after the LFO stage: FREQ only: %d %d", *slot(0, 8), *slot(0, 26));
    words_reset(10000);
    me_render_lfo(words);
    CHECK(*slot(0, 8) == 10000 + 64 * 32 * 4 && *slot(0, 26) == 10000, "LFO entry: L1.DEP only: %d %d", *slot(0, 8), *slot(0, 26));
    CHECK(*slot(0, 17) == 10000, "LFO entry: no bend");
    me_audio_cfg[0].mw_dest = 17;                 // TUNE as a destination: within TUNE's range
    words_reset(16384 + 20 * 256);
    mw_in(13, 127, AUTO);
    me_render(words);
    CHECK(*slot(0, 17) == TUNE_MAX, "TUNE route held at +24: %d", *slot(0, 17));

    // ---- the page ------------------------------------------------------------
    int ev[8];
    #define KEY(id_, down, rep) (memset(ev, 0, sizeof ev), ev[3] = (id_), ev[4] = (down) | ((rep) ? 8 : 0), me_page_key(0, ev))
    #define ENC(id_, n) (memset(ev, 0, sizeof ev), ev[3] = (id_), ev[4] = (n), me_page_enc(0, ev))
    #define OPEN() (h_trig_page = 1, KEY(19, 1, 0), KEY(19, 0, 0))

    // 25. A tap of TRIG on the TRIG page opens it; a hold, TRIG + a key, FUNC + TRIG or another page do not.
    reset();
    h_trig_page = 1;
    CHECK(KEY(19, 1, 0) == 0 && KEY(19, 0, 0) == 0, "TRIG reaches the stock page");
    CHECK(me_page_open, "tap of TRIG on the TRIG page: open");
    reset();
    h_trig_page = 1;
    KEY(19, 1, 0);
    for (int i = 0; i < TAP_TICKS; i++) me_page_tick(0);
    KEY(19, 0, 0);
    CHECK(!me_page_open, "a hold of TRIG: the stock page's");
    reset();
    h_trig_page = 1;
    KEY(19, 1, 0); KEY(9, 1, 0); KEY(9, 0, 0); KEY(19, 0, 0);
    CHECK(!me_page_open, "TRIG + REC: the stock copy, no page");
    reset();
    h_trig_page = 1; h_func_held = 1;
    KEY(19, 1, 0); KEY(19, 0, 0);
    CHECK(!me_page_open, "FUNC + TRIG: the quantize page");
    reset();
    KEY(19, 1, 0); h_trig_page = 1; KEY(19, 0, 0);
    CHECK(!me_page_open, "TRIG pressed on another page only brings the TRIG page");

    // 26. Audio cells: each knob its own setting, stepped through the accelerator
    //     (the stand-in gives 256 words a raw count: a whole step, or 1.00 of a depth).
    reset();
    h_active_track = 0;
    OPEN();
    CHECK(me_audio_cfg[0].mw_amount == 0 && me_audio_cfg[0].at_amount == 0 && me_audio_cfg[0].vel_amount == 0, "depths start at 0");
    CHECK(me_audio_cfg[0].vel_sens == 100 && me_audio_cfg[0].range == 2, "V.SNS 100 %%, BEND 2");
    CHECK(ENC(1, 3) == 1 && me_audio_cfg[0].range == 5, "A BEND 2 -> 5: %d", me_audio_cfg[0].range);
    CHECK(h_enc_last_sid == SID_TUNE, "BEND feels like TUNE");
    for (int i = 0; i < 20; i++) ENC(1, 1);
    CHECK(me_audio_cfg[0].range == PB_RANGE_MAX, "BEND at most 12: %d", me_audio_cfg[0].range);
    ENC(2, -30);
    CHECK(me_audio_cfg[0].vel_sens == 70, "B V.SNS 100 -> 70: %d", me_audio_cfg[0].vel_sens);
    ENC(2, -200);
    CHECK(me_audio_cfg[0].vel_sens == 0, "V.SNS at least 0");
    ENC(3, 1);
    CHECK(me_audio_cfg[0].mw_dest == 17 && list_cell == 2, "C MW: none -> SRC Tune, the list opens: %d %d", me_audio_cfg[0].mw_dest, list_cell);
    ENC(3, 6);
    CHECK(me_audio_cfg[0].mw_dest == 27, "MW: 6 more -> FLTR Resonance: %d", me_audio_cfg[0].mw_dest);
    KEY(42, 1, 0);
    CHECK(list_cell == -1 && me_page_open, "pushing knob C closes its list, the page stays");
    KEY(42, 0, 0);
    ENC(3, 1);
    CHECK(list_cell == 2, "turning again opens it again");
    KEY(41, 1, 0); KEY(41, 0, 0);
    CHECK(list_cell == 2, "another knob's push leaves it");
    ENC(4, -100);
    CHECK(me_audio_cfg[0].mw_amount == -AMOUNT_MAX && list_cell == -1, "D M.DEP to -64.00, the list closes: %d", me_audio_cfg[0].mw_amount);
    reset(); h_active_track = 0; OPEN();
    h_enc_fine = 1;                               // the accelerator's slow turn: a few words
    ENC(4, 1);
    CHECK(me_audio_cfg[0].mw_amount == 4, "a slow detent: 4/256 of a step (fine): %d", me_audio_cfg[0].mw_amount);
    h_enc_fine = 0;
    ENC(4, 3);
    CHECK(me_audio_cfg[0].mw_amount == 4 + 3 * 256, "a fast turn: whole steps: %d", me_audio_cfg[0].mw_amount);
    ENC(6, -10);
    CHECK(me_audio_cfg[0].vel_amount == -10 * 256, "F V.DEP 0 -> -10.00: %d", me_audio_cfg[0].vel_amount);
    ENC(5, 200);
    CHECK(me_audio_cfg[0].vel_dest == 16, "E VEL: to the end of the list, LFO2 Depth: %d", me_audio_cfg[0].vel_dest);
    ENC(7, 2); ENC(8, 5);
    CHECK(me_audio_cfg[0].at_dest == 19 && me_audio_cfg[0].at_amount == 5 * 256, "G AT, H A.DEP: %d %d", me_audio_cfg[0].at_dest, me_audio_cfg[0].at_amount);
    CHECK(me_audio_cfg[1].range == 2 && me_audio_cfg[1].mw_amount == 0, "track 2 unchanged");
    h_push_held = 1;
    ENC(6, 1);
    CHECK(me_audio_cfg[0].vel_amount == -6 * 256, "pushed: the push multiplier: %d", me_audio_cfg[0].vel_amount);
    h_push_held = 0;

    // 26a. FUNC + turn: the next useful value, then a rest of 0.4 s.
    reset(); h_active_track = 0; OPEN();
    h_func_held = 1;
    ENC(4, 1);
    CHECK(me_audio_cfg[0].mw_amount == 16 * 256, "M.DEP 0 -> +16: %d", me_audio_cfg[0].mw_amount);
    ENC(4, 1);
    CHECK(me_audio_cfg[0].mw_amount == 16 * 256, "resting: no second jump at once");
    for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    ENC(4, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    ENC(4, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    ENC(4, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    ENC(4, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].mw_amount == AMOUNT_MAX, "up to +64 and no further: %d", me_audio_cfg[0].mw_amount);
    me_audio_cfg[0].mw_amount = 20 * 256 + 37;    // between snaps
    ENC(4, -1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].mw_amount == 16 * 256, "20.14 down: +16: %d", me_audio_cfg[0].mw_amount);
    for (int i = 0; i < 6; i++) { ENC(4, -1); for (int j = 0; j < SNAP_REST; j++) me_page_tick(0); }
    CHECK(me_audio_cfg[0].mw_amount == -AMOUNT_MAX, "down to -64: %d", me_audio_cfg[0].mw_amount);
    me_audio_cfg[0].mw_amount = -20 * 256;
    ENC(4, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].mw_amount == -16 * 256, "-20 up: -16: %d", me_audio_cfg[0].mw_amount);
    ENC(1, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].range == 3, "BEND 2 -> 3: %d", me_audio_cfg[0].range);
    ENC(1, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    ENC(1, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].range == 7, "-> 5 -> 7: %d", me_audio_cfg[0].range);
    ENC(1, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].range == 12, "-> 12: %d", me_audio_cfg[0].range);
    ENC(2, -1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].vel_sens == 75, "V.SNS 100 -> 75: %d", me_audio_cfg[0].vel_sens);
    ENC(3, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].mw_dest == 17, "MW none -> SRC: %d", me_audio_cfg[0].mw_dest);
    ENC(3, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].mw_dest == 26, "SRC -> FLTR Frequency: %d", me_audio_cfg[0].mw_dest);
    ENC(3, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    ENC(3, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].mw_dest == 1, "-> AMP -> LFO1 Speed: %d", me_audio_cfg[0].mw_dest);
    me_audio_cfg[0].mw_dest = 27;                 // FLTR Resonance
    ENC(3, -1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].mw_dest == 26, "back: the first of FLTR: %d", me_audio_cfg[0].mw_dest);
    ENC(3, -1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].mw_dest == 17, "back again: SRC: %d", me_audio_cfg[0].mw_dest);
    ENC(3, -1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].mw_dest == 0, "and none: %d", me_audio_cfg[0].mw_dest);
    h_func_held = 0;

    CHECK(ENC(9, 3) == 0, "LEVEL stays the track level");
    CHECK(KEY(30, 1, 0) == 0 && KEY(10, 1, 0) == 0 && KEY(2, 1, 0) == 0, "trig keys, PLAY and TRK go to the stock page");
    CHECK(KEY(14, 1, 0) == 1 && KEY(12, 1, 0) == 1, "arrows and YES: taken, nothing happens");
    CHECK(me_page_open, "still open");
    for (int i = 0; i < SHOW_TICKS; i++) me_page_tick(0);
    me_audio_cfg[0].at_amount = 12 * 256 + 128;
    ENC(8, 0);
    shown[7] = 1;
    memset(drawn, 0, sizeof drawn);
    h_knob_calls = 0;
    me_page_draw((void *)1, (void *)1);
    CHECK(strstr(drawn, "Expression") && strstr(drawn, "BEND") && strstr(drawn, "V.SNS") && strstr(drawn, "+12.50") && strstr(drawn, "M.DEP"), "drawn: %s", drawn);
    CHECK(h_knob_calls == 4, "four knobs (V.SNS and the three depths): %d", h_knob_calls);
    me_audio_cfg[0].at_amount = -5 * 256;
    shown[7] = 1;
    memset(drawn, 0, sizeof drawn);
    me_page_draw((void *)1, (void *)1);
    CHECK(strstr(drawn, "-5.00"), "negative depth: %s", drawn);
    me_audio_cfg[0].mw_dest = 0;
    ENC(3, 2);
    memset(drawn, 0, sizeof drawn);
    me_page_draw((void *)1, (void *)1);
    CHECK(strstr(drawn, "None") && strstr(drawn, "SRC:Name"), "the list: %s", drawn);
    for (int i = 0; i < LIST_TICKS; i++) me_page_tick(0);
    CHECK(list_cell == -1, "the list closes by itself");
    KEY(13, 1, 0);
    CHECK(!me_page_open, "NO leaves");

    // 26b. The safety net: an accelerator that gives nothing for six detents gives way to a step a detent.
    reset();
    h_active_track = 0;
    OPEN();
    h_enc_zero = 1;
    for (int i = 0; i < 5; i++) ENC(2, -4);
    CHECK(me_audio_cfg[0].vel_sens == 100, "five detents of nothing: unchanged (the stock slow start)");
    ENC(2, -4);
    CHECK(me_audio_cfg[0].vel_sens == 99, "the sixth: a step: %d", me_audio_cfg[0].vel_sens);
    ENC(2, -8);
    CHECK(me_audio_cfg[0].vel_sens == 97, "then a step a detent: %d", me_audio_cfg[0].vel_sens);
    ENC(4, -4); ENC(4, -4); ENC(4, -4); ENC(4, -4); ENC(4, -4); ENC(4, -4);
    CHECK(me_audio_cfg[0].mw_amount == -256, "a depth too: 1.00 a detent: %d", me_audio_cfg[0].mw_amount);
    h_enc_zero = 0;
    ENC(2, -1);
    CHECK(me_audio_cfg[0].vel_sens == 96 && idle_counts[1] == 0, "the accelerator back: %d", me_audio_cfg[0].vel_sens);

    // 27. MIDI cells: THRU, V.SNS, three destinations from the LFO page's list, fine depths.
    reset();
    h_active_track = 8;
    pb_in(13, 12000, AUTO);
    OPEN();
    before = nsent;
    ENC(1, -1);
    CHECK(me_midi_cfg[0].thru == 0, "A THRU off");
    CHECK(nsent == before + 1 && sent[nsent - 1][0] == 0xE0 && pb_of(nsent - 1) == 8192, "the synth's bend back to the track's: %d", nsent - before);
    ENC(1, 1);
    CHECK(me_midi_cfg[0].thru == 1, "A THRU on again");
    ENC(2, -50);
    CHECK(me_midi_cfg[0].vel_sens == 50, "B V.SNS 50: %d", me_midi_cfg[0].vel_sens);
    ENC(3, 3);
    CHECK(me_midi_cfg[0].mw_dest == 3 && list_cell == 2, "C MW -> SRC Mod Wheel (3rd), the list opens: %d", me_midi_cfg[0].mw_dest);
    ENC(3, 100);
    CHECK(me_midi_cfg[0].mw_dest == 12, "to the end: CC VAL8: %d", me_midi_cfg[0].mw_dest);
    SEL(0, 7, 74);
    memset(drawn, 0, sizeof drawn);
    me_page_draw((void *)1, (void *)1);
    CHECK(strstr(drawn, "CC:CC8 Value #74") && strstr(drawn, "#74") && strstr(drawn, "VAL8"), "the list and box show SEL 74: %s", drawn);
    KEY(42, 1, 0); KEY(42, 0, 0);
    h_func_held = 1;
    ENC(3, -1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_midi_cfg[0].mw_dest == 5, "FUNC back: the first CC slot: %d", me_midi_cfg[0].mw_dest);
    ENC(3, -1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_midi_cfg[0].mw_dest == 1, "then SRC Pitch Bend: %d", me_midi_cfg[0].mw_dest);
    ENC(3, -1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_midi_cfg[0].mw_dest == 0, "then None: %d", me_midi_cfg[0].mw_dest);
    ENC(4, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_midi_cfg[0].mw_amount == 16 * 256, "M.DEP +16: %d", me_midi_cfg[0].mw_amount);
    h_func_held = 0;
    ENC(5, 1); ENC(6, 2); ENC(7, 2); ENC(8, -3);
    CHECK(me_midi_cfg[0].vel_dest == 1 && me_midi_cfg[0].vel_amount == 512 && me_midi_cfg[0].at_dest == 2 && me_midi_cfg[0].at_amount == -768,
          "E-H: VEL -> PB +2.00, AT -> AT -3.00");
    CHECK(me_midi_cfg[1].vel_dest == 0 && me_midi_cfg[1].thru == 1, "MIDI B unchanged");
    for (int i = 0; i < SHOW_TICKS + LIST_TICKS; i++) me_page_tick(0);
    memset(drawn, 0, sizeof drawn);
    me_page_draw((void *)1, (void *)1);
    CHECK(strstr(drawn, "THRU") && strstr(drawn, "V.SNS") && strstr(drawn, "M.DEP") && strstr(drawn, "SRC") && strstr(drawn, "PB"), "MIDI drawn: %s", drawn);

    // ---- MIDI routing, merging and whacky combinations -------------------------
    // 29. Routes add to their destination: wheel -> CC VAL1 (SEL 74), +32.00 = half the range at full wheel.
    reset();
    SEL(0, 0, 74);
    me_midi_cfg[0].mw_dest = 5; me_midi_cfg[0].mw_amount = 32 * 256;
    track_id(0, 25, 10);                          // VAL1's own value 10
    track_mw(0, 20);                              // MW knob 20
    mw_in(13, 127, AUTO);
    CHECK(last_cc(74) == 74, "VAL1: 10 + 127 x 32/64 = 73.5 -> 74: %d", last_cc(74));
    CHECK(last_cc(1) == 127, "MW (THRU): 20 + 127 capped: %d", last_cc(1));
    CHECK(track_id(0, 25, 11) == TAKE_OVER && last_cc(74) == 75, "VAL1's own sends taken over, the sum follows: %d", last_cc(74));
    CHECK(track_id(0, 26, 50) == (50 << 8), "VAL2, nothing aimed at it: stock");
    me_midi_cfg[0].mw_amount = -64 * 256;
    me_midi_changed(0);
    CHECK(last_cc(74) == 0, "-64.00 at full wheel: down to 0: %d", last_cc(74));

    // 30. Wheel to pitch bend (with THRU's own bend), aftertouch to the mod wheel, velocity to BC.
    reset();
    me_midi_cfg[0].mw_dest = 1; me_midi_cfg[0].mw_amount = 64 * 256;
    me_midi_cfg[0].at_dest = 3; me_midi_cfg[0].at_amount = 32 * 256;
    me_midi_cfg[0].vel_dest = 4; me_midi_cfg[0].vel_amount = 64 * 256;
    pb_in(13, 8192 + 1000, AUTO);
    mw_in(13, 64, AUTO);
    {
        int pb = -1; for (int i = 0; i < nsent; i++) if (sent[i][0] == 0xE0) pb = pb_of(i);
        CHECK(pb == clamp(8192 + 1000 + 64 * 64 * 256 / 64 * 16383 / 32512, 0, 16383), "PB = centre + bend + wheel x 64: %d", pb);
    }
    CHECK(last_cc(1) == 64, "MW: THRU's wheel, 64: %d", last_cc(1));
    at_in(13, 100, AUTO);
    CHECK(last_cc(1) == 64 + 50, "MW: wheel 64 + pressure 100 x 32/64: %d", last_cc(1));
    {
        int at = -1; for (int i = 0; i < nsent; i++) if (sent[i][0] == 0xD0) at = sent[i][1];
        CHECK(at == 100, "AT: THRU's own pressure too: %d", at);
    }
    uint8_t note[3] = { 0x90, 60, 100 };
    before = nsent;
    me_note_out(3, note);
    CHECK(nsent == before + 1 && last_cc(2) == 100, "velocity 100 -> BC at once (before the note): %d", last_cc(2));
    before = nsent;
    h_active_track = 0;
    me_tick(0);
    CHECK(last_cc(1) == 0, "track change: MW back to the track's own (0): %d", last_cc(1));
    CHECK(last_cc(2) == 100, "BC keeps the note's velocity (the sequencer's notes set it)");

    // 31. Merging: VAL1 on CC 1 (SEL 1) with the mod wheel: one CC 1, the latest own value + everything live.
    reset();
    SEL(0, 0, 1);
    me_midi_cfg[0].mw_dest = 5; me_midi_cfg[0].mw_amount = 16 * 256;   // wheel -> VAL1 (= CC 1) too
    track_mw(0, 20);                              // SRC MW knob 20
    track_id(0, 25, 40);                          // VAL1 40, changed last
    before = nsent;
    mw_in(13, 64, AUTO);                          // THRU wheel 64 on MW, and x 16/64 = 16 on VAL1
    CHECK(count_cc(before, 1) == 1, "one CC 1 for both: %d", count_cc(before, 1));
    CHECK(last_cc(1) == 40 + 64 + 16, "VAL1's own 40 (the latest) + 64 + 16: %d", last_cc(1));
    CHECK(track_mw(0, 30) == TAKE_OVER && last_cc(1) == 30 + 80, "the MW knob moves: now it is the latest: %d", last_cc(1));
    CHECK(track_id(0, 25, 41) == TAKE_OVER && last_cc(1) == 41 + 80, "VAL1 moves: it again: %d", last_cc(1));
    // an LFO on both, alternating: every send is taken over, every value valid, one CC 1 each
    for (int i = 0; i < 200; i++) {
        before = nsent;
        if (i & 1) track_mw(0, (i * 7) % 128); else track_id(0, 25, (i * 13) % 128);
        if (nsent > 200) nsent = 0;
        CHECK(count_cc(before > nsent ? 0 : before, 1) <= 1, "one CC 1 a send");
    }
    for (int i = 0; i < nsent; i++) for (int j = 1; j < sent_len[i]; j++) CHECK(sent[i][j] <= 127, "data byte <= 127");
    // the wheel goes away (track change): CC 1 back to the latest own value, then stock again
    nsent = 0;
    track_id(0, 25, 55);
    h_active_track = 0;
    me_tick(0);
    CHECK(last_cc(1) == 55, "released: the latest own value: %d", last_cc(1));
    CHECK(track_mw(0, 3) == (3 << 8) && track_id(0, 25, 4) == (4 << 8), "then both stock again");

    // 32. SEL changed while merged; two slots on one CC; slot on CC 2 merges with BC; SEL off (-1).
    reset();
    SEL(0, 0, 1);
    track_id(0, 25, 10);
    mw_in(13, 50, AUTO);
    CHECK(last_cc(1) == 60, "merged: %d", last_cc(1));
    SEL(0, 0, 74);
    before = nsent;
    mw_in(13, 51, AUTO);
    CHECK(count_cc(before, 1) == 1 && last_cc(1) == 51 && count_cc(before, 74) == 0, "SEL now 74: CC 1 is the wheel alone; VAL1 not live: %d", last_cc(1));
    CHECK(track_id(0, 25, 12) == (12 << 8), "VAL1 is stock again");
    reset();
    SEL(0, 2, 30); SEL(0, 5, 30);                 // VAL3 and VAL6 both on CC 30
    me_midi_cfg[0].at_dest = 7; me_midi_cfg[0].at_amount = 64 * 256;   // pressure -> VAL3 (dest 7)
    track_id(0, 27, 5); track_id(0, 30, 9);       // VAL3 5, VAL6 9 (latest)
    before = nsent;
    at_in(13, 20, AUTO);
    CHECK(count_cc(before, 30) == 1 && last_cc(30) == 9 + 20, "two slots on CC 30: one message, VAL6's 9 + 20: %d", last_cc(30));
    CHECK(track_id(0, 27, 6) == TAKE_OVER && track_id(0, 30, 7) == TAKE_OVER, "both taken over");
    reset();
    SEL(0, 1, 2);                                 // VAL2 on CC 2: merges with BC
    track_id(0, 24, 30);
    track_id(0, 26, 70);
    bc_in(13, 40, AUTO);
    CHECK(last_cc(2) == 110 && count_cc(0, 2) == 1, "BC thru + VAL2's own 70 (latest): %d", last_cc(2));
    reset();
    SEL(0, 0, -1);                                // no CC number
    me_midi_cfg[0].mw_dest = 5; me_midi_cfg[0].mw_amount = 64 * 256;
    mw_in(13, 100, AUTO);
    for (int i = 0; i < nsent; i++) CHECK(sent[i][1] <= 127, "SEL off: nothing invalid: %02x %d", sent[i][0], sent[i][1]);

    // 33. Mod wheel on mod wheel: THRU and a route to MW: added twice; -64 cancels it.
    reset();
    me_midi_cfg[0].mw_dest = 3; me_midi_cfg[0].mw_amount = 32 * 256;
    track_mw(0, 10);
    mw_in(13, 40, AUTO);
    CHECK(last_cc(1) == 10 + 40 + 20, "MW: own 10 + 40 + 40 x 32/64: %d", last_cc(1));
    me_midi_cfg[0].mw_amount = -64 * 256;
    me_midi_changed(0);
    CHECK(last_cc(1) == 10, "-64.00: the wheel cancelled: %d", last_cc(1));
    me_midi_cfg[0].thru = 0;
    me_midi_changed(0);
    CHECK(last_cc(1) == 0, "THRU off, route -64 alone: 10 - 40 capped at 0: %d", last_cc(1));

    // 34. Bend routed into bend: pitch bend's own source is THRU only; pressure -> PB down, below 0 capped.
    reset();
    me_midi_cfg[0].at_dest = 1; me_midi_cfg[0].at_amount = -64 * 256;
    track_pb(0, 1000);
    pb_in(13, 0, AUTO);                           // -8192
    at_in(13, 127, AUTO);
    {
        int pb = -1; for (int i = 0; i < nsent; i++) if (sent[i][0] == 0xE0) pb = pb_of(i);
        CHECK(pb == 0, "1000 - 8192 - full: capped at 0: %d", pb);
    }

    // 35. Two MIDI tracks on one channel: the one that last took input owns the settings.
    reset();
    chan_of[1] = 0;                               // MIDI B also on channel 1
    me_midi_cfg[1].thru = 0;
    mw_in(8, 50, TRACK);                          // A's track channel 9
    CHECK(last_cc(1) == 50, "A: THRU on: %d", last_cc(1));
    before = nsent;
    mw_in(9, 60, TRACK);                          // B's track channel 10: B's settings (THRU off)
    CHECK(last_cc(1) == 0, "B owns the channel now: THRU off: the track's own (0): %d", last_cc(1));

    // 36. Fuzz: random settings, SEL numbers, inputs, track sends, notes, track changes and
    //     setting changes; every message valid, at most one per CC per send, and with nothing
    //     live at the end, everything the mod took is back to the track's own values.
    {
        #define RND(n) rnd_next(n)
        uint32_t sent0 = me_sent, filt0 = me_filtered, taken_over = 0, merged = 0;
        for (int round = 0; round < 300; round++) {
            reset();
            for (int m = 0; m < 2; m++) {
                me_midi_cfg[m].thru = (int8_t)RND(2);
                me_midi_cfg[m].mw_dest = (int8_t)RND(13); me_midi_cfg[m].at_dest = (int8_t)RND(13); me_midi_cfg[m].vel_dest = (int8_t)RND(13);
                me_midi_cfg[m].mw_amount = (int16_t)(RND(2 * AMOUNT_MAX + 1) - AMOUNT_MAX);
                me_midi_cfg[m].at_amount = (int16_t)(RND(2 * AMOUNT_MAX + 1) - AMOUNT_MAX);
                me_midi_cfg[m].vel_amount = (int16_t)(RND(2 * AMOUNT_MAX + 1) - AMOUNT_MAX);
                me_midi_cfg[m].vel_sens = (int8_t)RND(101);
                for (int k = 0; k < 8; k++) SEL(m, k, RND(5) == 0 ? 1 : RND(5) == 0 ? 2 : RND(8) == 0 ? -1 : RND(128));
            }
            chan_of[1] = RND(2) ? 0 : 5;
            int own[OUTPUTS];
            for (int o = 0; o < OUTPUTS; o++) own[o] = -1;
            for (int step = 0; step < 60; step++) {
                int what = RND(10), from = nsent;
                if (nsent > 200) { nsent = 0; from = 0; }
                switch (what) {
                case 0: pb_in(RND(2) ? 13 : 8, RND(16384), RND(2) ? AUTO : TRACK); break;
                case 1: at_in(RND(2) ? 13 : 8, RND(128), RND(2) ? AUTO : TRACK); break;
                case 2: mw_in(RND(2) ? 13 : 8, RND(128), RND(2) ? AUTO : TRACK); break;
                case 3: bc_in(RND(2) ? 13 : 8, RND(128), RND(2) ? AUTO : TRACK); break;
                case 4: case 5: {
                    int o = RND(OUTPUTS), v = o == O_PB ? RND(16384) : RND(128);
                    own[o] = v;
                    if (track_id(0, ID_FIRST + o, v) == TAKE_OVER) {
                        taken_over++;
                        int32_t cc = cc_of(&chans[0], o);
                        for (int p2 = O_MW; p2 < OUTPUTS; p2++)
                            if (p2 != o && cc >= 0 && cc_of(&chans[0], p2) == cc) { merged++; break; }
                    }
                    break;
                }
                case 6: { uint8_t n[3] = { 0x90, (uint8_t)RND(128), (uint8_t)(1 + RND(127)) }; const uint8_t *r2 = me_note_out(3, n);
                          CHECK(r2[2] >= n[2] && r2[2] <= 127, "note velocity scaled within %d..127: %d", n[2], r2[2]); break; }
                case 7: h_active_track = RND(3) ? 8 : RND(16); me_tick(0); break;
                case 8: me_midi_cfg[0].mw_dest = (int8_t)RND(13); me_midi_cfg[0].thru = (int8_t)RND(2); me_midi_changed(0); break;
                case 9: SEL(0, RND(8), RND(3) ? RND(128) : 1); me_tick(0); break;
                }
                for (int i = from; i < nsent; i++) {
                    CHECK(sent[i][0] >= 0x80 && sent[i][0] < 0xF0, "a channel message: %02x", sent[i][0]);
                    for (int j = 1; j < sent_len[i]; j++) CHECK(sent[i][j] <= 127, "data byte: %d", sent[i][j]);
                }
                for (int cc = 0; cc < 128; cc++) {
                    int n = 0;
                    for (int i = from; i < nsent; i++) if (sent[i][0] == 0xB0 && sent[i][1] == cc) n++;
                    CHECK(n <= 2, "CC %d sent %d times by one event", cc, n);
                }
            }
            // everything live goes: track change away from MIDI, inputs on the auto channel only
            for (int ch = 0; ch < 16; ch++) { chans[ch].via_auto = chans[ch].touched & (uint8_t)~BIT(S_VEL); }
            h_active_track = 1; me_tick(0); h_active_track = 2; me_tick(0);
            for (int m = 0; m < 8; m++) { me_midi_cfg[m].vel_dest = 0; me_midi_changed(m); }
            for (int ch = 0; ch < 16; ch++)
                for (int o = 0; o < OUTPUTS; o++)
                    CHECK(!group_live(&chans[ch], o), "round %d: ch %d output %d still live", round, ch, o);
            nsent = 0;
            for (int o = 0; o < OUTPUTS; o++)
                if (own[o] >= 0)
                    CHECK(track_id(0, ID_FIRST + o, own[o]) != TAKE_OVER, "round %d: output %d back to stock", round, o);
        }
        printf("fuzz: %u messages sent, %u track sends seen, %u taken over, %u of them merged\n",
               me_sent - sent0, me_filtered - filt0, taken_over, merged);
    }

    // 28. Leaving: another page key, the page gone from under it, TRIG again.
    reset();
    OPEN();
    CHECK(KEY(21, 1, 0) == 0 && !me_page_open, "FLTR: stock takes it, the page closes");
    OPEN();
    CHECK(me_page_open, "open again");
    h_trig_page = 0;
    me_page_draw((void *)1, (void *)1);
    CHECK(!me_page_open, "the TRIG page gone: closed");
    reset();
    OPEN();
    CHECK(KEY(19, 1, 0) == 1 && me_page_open && KEY(19, 0, 0) == 1 && !me_page_open, "TRIG again: back to the first page");
    CHECK(ENC(1, 1) == 0, "closed: knobs are stock's");

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
