// Host unit tests for MIDI Expression's logic (module/midi-expression/src/expr.c).
//   cc -std=c11 -Wall -DME_HOST_TEST -I tests/host tests/host/test_expr.c -o build/test_expr && build/test_expr
#include <stdio.h>
#include <string.h>

#include "../../module/midi-expression/src/expr.c"
static int16_t h_os_vel[8];
#define OS_VEL h_os_vel
#include "../../module/midi-expression/src/audio.c"
#include "../../module/midi-expression/src/page.c"
#include "../../module/midi-expression/src/store.c"
#include <stdarg.h>

volatile int32_t h_auto_channel = 13, h_active_track = 8, h_track_channel[16], h_receive_cc = 1;
volatile uint32_t h_output_to = 3;
volatile uint8_t h_notes[1 + 1024], h_ccs[1 + 1024];
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
static uint8_t h_off[8][64];                 // a parameter switched off (FUNC + knob)
int32_t h_enabled(int32_t t, int32_t id) { return t >= 0 && t < 8 && id >= 0 && id < 64 ? !h_off[t][id] : 1; }
int h_cc_appends;
void h_cc_append(int32_t len, const uint8_t *b)   // 0x400e0dac: no length check, as stock
{
    for (int i = 0; i < len; i++) h_ccs[1 + h_ccs[0] + i] = b[i];
    h_ccs[0] = (uint8_t)(h_ccs[0] + len);
    h_cc_appends++;
}
// The task's buffers as midi_out sends them: message by message into sent[].
static void drain(volatile uint8_t *buf)
{
    int n = buf[0];
    for (int i = 0; i < n; ) {
        int len = (buf[1 + i] & 0xE0) == 0xC0 ? 2 : 3;
        if (nsent < 256) {
            for (int j = 0; j < len; j++) sent[nsent][j] = buf[1 + i + j];
            sent_len[nsent++] = len;
        }
        i += len;
    }
    buf[0] = 0;
}
// The MIDI tracks' task: with h_task_auto, a posted wake is served at once
// (the task's pass, then its buffers); otherwise it waits for task().
static int h_task_auto = 1, h_queued, h_in_queue_max, h_task_busy;
static void task(void)
{
    while (h_queued) {
        h_queued--;
        h_task_busy = 1;
        me_task_pass();
        drain(h_notes);
        drain(h_ccs);
        h_task_busy = 0;
    }
}
void h_queue_send(void *q, void *item)
{
    (void)q; (void)item;
    h_queued++;
    if (h_queued > h_in_queue_max) h_in_queue_max = h_queued;
    if (h_task_auto && !h_task_busy) task();
}
// notes_append's caller: the notes buffer goes out after the pass (sent[] in order).
static const uint8_t *note_out(int32_t len, const uint8_t *b)
{
    const uint8_t *r = me_note_out(len, b);
    drain(h_notes);
    return r;
}
void h_stock_cc(const uint8_t *msg, int32_t len, int32_t flag) { (void)msg; (void)len; (void)flag; stock_cc_calls++; }

static uint32_t rnd_state = 12345;
static int rnd_next(int n) { rnd_state = rnd_state * 1103515245u + 12345u; return (int)((rnd_state >> 16) % (uint32_t)n); }

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void reset(void)
{
    memset((void *)chans, 0, sizeof chans);
    initialised = 0; posted = 0; last_active = -1;
    h_notes[0] = h_ccs[0] = 0; h_task_auto = 1; h_queued = 0; h_in_queue_max = 0; h_task_busy = 0;
    clock_stamp = 0;
    memset((void *)h_params, 0, sizeof h_params);
    memset(h_off, 0, sizeof h_off);
    for (int m = 0; m < 8; m++) h_params[106 * m + 2 * 21] = 0x20;   // PB's parameter at its centre, 0x2000
    for (int t = 0; t < 16; t++) h_track_channel[t] = t;   // tracks 1-8 ch 1-8, A-H ch 9-16
    for (int m = 0; m < 8; m++) chan_of[m] = -1;
    chan_of[0] = 0;                                        // MIDI track A: CHAN on, channel 1
    h_active_track = 8; h_auto_channel = 13;
    nsent = 0; stock_cc_calls = 0;
    memset((void *)atracks, 0, sizeof atracks);
    ALL(range, 2 * 256); ALL(mw_dest, 0); ALL(at_dest, 0); ALL(vel_dest, 0); ALL(mw_amount, 0); ALL(at_amount, 0); ALL(vel_amount, 0); ALL(vel_sens, 100);
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

    // 9. Only the task sends: input posts one wake, however much comes in; the newest value goes in its pass.
    reset();
    h_task_auto = 0;
    pb_in(13, 9000, AUTO);
    pb_in(13, 10000, AUTO);
    mw_in(13, 40, AUTO);
    me_tick(0);
    CHECK(nsent == 0 && h_ccs[0] == 0, "nothing sent outside the task: %d", nsent);
    CHECK(h_queued == 1, "one wake in the queue: %d", h_queued);
    task();
    CHECK(nsent == 2 && pb_of(0) == 10000 && last_cc(1) == 40, "the pass: newest PB and MW: %d sent", nsent);
    me_tick(0);
    CHECK(h_queued == 0, "nothing pending: no wake");
    // The buffer's room: with the CC buffer near ROOM, what does not fit waits for the next pass.
    reset();
    h_task_auto = 0;
    for (int m = 1; m < 8; m++) chan_of[m] = m;
    for (int ch = 0; ch < 8; ch++) h_track_channel[8 + ch] = 5;   // A-H all on channel 6
    h_ccs[0] = ROOM - 8;                                            // stock's bytes: room for two
    for (int i = 0; i < ROOM - 8; i++) h_ccs[1 + i] = (i % 3) ? 0 : 0xB5;
    pb_in(5, 3000, TRACK);
    int stock_msgs = (ROOM - 8) / 3;
    h_queued--; me_task_pass(); drain(h_notes); drain(h_ccs);   // one record, one pass
    CHECK(nsent == stock_msgs + 2, "first pass: stock's bytes and two of eight: %d", nsent - stock_msgs);
    CHECK(h_queued == 1 && posted, "and asks for another pass");
    task();
    CHECK(nsent == stock_msgs + 8, "next pass: the other six: %d", nsent - stock_msgs);
    for (int i = stock_msgs; i < nsent; i++) CHECK(sent[i][0] >= 0xE0 && sent[i][0] < 0xE8, "a bend on A-H's channel: %02x", sent[i][0]);
    CHECK(h_in_queue_max <= 1, "never more than one wake queued: %d", h_in_queue_max);
    CHECK(h_cc_appends >= 8, "through the task's CC append (FX Track sees them): %d", h_cc_appends);

    // 9b. The task calls send() again and again with an unchanged value (hardware: its LFO,
    //     every pass): what did not change is not sent again; a change, or a forced send, is.
    reset();
    mw_in(13, 50, AUTO);
    before = nsent;
    for (int i = 0; i < 20; i++) CHECK(track_mw(0, 10) == TAKE_OVER, "taken over");
    CHECK(nsent == before + 1 && last_cc(1) == 60, "20 unchanged sends: one CC 1 = 60: %d", nsent - before);
    before = nsent;
    for (int i = 0; i < 5; i++) mw_in(13, 50, AUTO);
    CHECK(nsent == before, "the wheel again at 50: nothing: %d", nsent - before);
    track_mw(0, 11);
    CHECK(nsent == before + 1 && last_cc(1) == 61, "a change goes: %d", last_cc(1));
    before = nsent;
    me_filter(0, 0, ID_MW, 11 << 8, 1);
    CHECK(nsent == before + 1 && last_cc(1) == 61, "a forced send goes: %d", nsent - before);
    mw_in(13, 0, AUTO);
    before = nsent;
    for (int i = 0; i < 20; i++) track_mw(0, 11);
    CHECK(nsent == before, "wheel 0, THRU still on it: own 11 not repeated: %d", nsent - before);
    // stock sent in between (nothing live): the next live value goes even if it matches the last.
    reset();
    mw_in(13, 50, AUTO);
    track_mw(0, 10);
    me_midi_cfg[0].thru = 0; me_midi_changed(0);         // back to stock: CC 1 = 10
    CHECK(track_mw(0, 30) != TAKE_OVER, "stock again");
    me_midi_cfg[0].thru = 1;
    before = nsent;
    me_midi_changed(0);
    CHECK(nsent == before + 1 && last_cc(1) == 30 + 50, "live again: own 30 + 50: %d", last_cc(1));
    track_mw(0, 10);
    CHECK(last_cc(1) == 60, "then 10 + 50: %d", last_cc(1));
    me_midi_cfg[0].thru = 0; me_midi_changed(0);
    track_mw(0, 60);                                      // stock sends 60 itself
    me_midi_cfg[0].thru = 1;
    before = nsent;
    mw_in(13, 0, AUTO);                                   // live: 60 + 0, as stock's last, but the mod must not assume
    CHECK(nsent == before + 1 && last_cc(1) == 60, "the mod's 60 goes though stock's was 60 too (unknown): %d", nsent - before);

    // 9c. Before the track has sent a value, its own is the track's parameter in use (FLTR VAL1 = 10, MW = 10).
    reset();
    h_params[2 * 23] = 10; h_params[2 * 25] = 10; SEL(0, 0, 1); h_params[2 * 22] = 5;
    mw_in(13, 50, AUTO);
    CHECK(last_cc(1) == 60, "MW param 10 + wheel 50 = 60 before any send: %d", last_cc(1));
    at_in(13, 20, AUTO);
    CHECK(nsent && sent[nsent - 1][0] == 0xD0 && sent[nsent - 1][1] == 25, "AT param 5 + 20 = 25: %02x %d", sent[nsent - 1][0], sent[nsent - 1][1]);
    track_mw(0, 30);
    CHECK(last_cc(1) == 80, "then the track's send counts: 30 + 50: %d", last_cc(1));
    pb_in(13, 9000, AUTO);
    CHECK(pb_of(nsent - 1) == 9000, "PB: the parameter's centre 0x2000 + 808: %d", pb_of(nsent - 1));
    h_params[2 * 21] = 0x30; h_params[2 * 21 + 1] = 0x00;            // PB knob up: 12288
    pb_in(13, 8192 + 100, AUTO);
    CHECK(pb_of(nsent - 1) == 12288 + 100, "PB parameter 12288 + 100: %d", pb_of(nsent - 1));

    // 9d. MIDI LFO as a destination: added just before the LFO runs, the words back after.
    reset();
    {
        static int16_t lp[8 * 53];
        for (int i = 0; i < 8 * 53; i++) lp[i] = (int16_t)(i * 3);
        lp[1] = 0x7000; lp[2] = 0x0400; lp[8] = 0x4000;              // track A: SPD 48, MULT 4, DEP 0
        me_midi_cfg[0].mw_dest = MIDI_LFO_FIRST + 0; me_midi_cfg[0].mw_amount = 16 * 256;   // wheel -> SPD +16
        me_midi_cfg[0].at_dest = MIDI_LFO_FIRST + 4; me_midi_cfg[0].at_amount = -64 * 256;  // pressure -> DEP -64
        me_midi_cfg[0].vel_dest = MIDI_LFO_FIRST + 4; me_midi_cfg[0].vel_amount = 32 * 256; // velocity -> DEP +32
        me_midi_lfo_pre(lp, 0);
        CHECK(lp[1] == 0x7000 && lp[8] == 0x4000, "nothing live: untouched");
        me_midi_lfo_post(lp, 0);
        me_midi_cfg[0].thru = 0;
        before = nsent;
        mw_in(13, 20, AUTO); at_in(13, 64, AUTO);
        CHECK(nsent == before, "an LFO route sends nothing itself: %d", nsent - before);
        me_midi_lfo_pre(lp, 0);
        CHECK(lp[1] == 0x7000 + 20 * 16 * 256 / 64 && lp[8] == 0x4000 - 64 * 64 * 256 / 64,
              "SPD + wheel x 16, DEP - pressure x 64: %04x %04x", (uint16_t)lp[1], (uint16_t)lp[8]);
        CHECK(lp[2] == 0x0400 && lp[53 + 1] == 54 * 3 - 3 + 3, "MULT and track B untouched");
        lp[8] = (int16_t)(lp[8] + 100);                                 // the function wrote it itself
        me_midi_lfo_post(lp, 0);
        CHECK(lp[1] == 0x7000 && lp[8] == 0x4000 + 100, "back after (keeping the function's own change): %04x %04x", (uint16_t)lp[1], (uint16_t)lp[8]);
        mw_in(13, 127, AUTO); at_in(13, 127, AUTO);
        me_midi_lfo_pre(lp, 0);
        CHECK(lp[1] == 32512 && lp[8] == 0, "held within 0..32512: %04x %04x", (uint16_t)lp[1], (uint16_t)lp[8]);
        me_midi_lfo_post(lp, 0);
        int16_t was1 = lp[53 + 1];
        me_midi_lfo_pre(lp, 1); me_midi_lfo_post(lp, 1);
        CHECK(lp[53 + 1] == was1, "track B (CHAN off): untouched");
        me_midi_cfg[0].mw_dest = 0; me_midi_cfg[0].at_dest = 0; me_midi_cfg[0].vel_dest = 0;
    }

    // 9e. A parameter switched off on its page (FUNC + knob): its own value is gone, the wheel runs from 0.
    reset();
    track_mw(0, 64);                                      // SRC MW 64, sent
    mw_in(13, 10, AUTO);
    CHECK(last_cc(1) == 74, "MW 64 + wheel 10: %d", last_cc(1));
    h_off[0][ID_MW] = 1;                                  // FUNC + G: MW off
    mw_in(13, 0, AUTO);
    CHECK(last_cc(1) == 0, "MW off: wheel 0 sends 0: %d", last_cc(1));
    mw_in(13, 30, AUTO);
    CHECK(last_cc(1) == 30, "and 30 is 30: %d", last_cc(1));
    h_params[2 * 25] = 50; SEL(0, 0, 1);                  // VAL1 on CC 1, 50, then switched off too
    h_off[0][25] = 1;
    mw_in(13, 5, AUTO);
    CHECK(last_cc(1) == 5, "VAL1 on CC 1 off as well: 5: %d", last_cc(1));
    h_off[0][25] = 0;
    mw_in(13, 6, AUTO);
    CHECK(last_cc(1) == 56, "VAL1 on again: its 50 + 6: %d", last_cc(1));

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

    // 11. OUTPUT TO: the task's midi_out sends the buffer where it says, the mod's bytes with stock's.
    reset();
    h_output_to = 2;
    pb_in(13, 9000, AUTO);
    CHECK(nsent == 1, "OUTPUT TO USB only: into the buffer, as stock's: %d", nsent);
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
    ALL(range, 12 * 256);
    words_reset(16384);
    pb_in(13, 16383, AUTO);                       // full up: 8191/8192 of 12 semitones
    me_render(words);
    CHECK(*slot(0, 17) == 16384 + 3071, "TUNE +12: %d", *slot(0, 17) - 16384);
    words_reset(16384);
    pb_in(13, 0, AUTO);
    me_render(words);
    CHECK(*slot(0, 17) == 16384 - 3072, "TUNE -12: %d", *slot(0, 17) - 16384);
    ALL(range, 24 * 256);                           // more than the row allows: held to 12
    words_reset(16384);
    pb_in(13, 0, AUTO);
    me_render(words);
    CHECK(*slot(0, 17) == 16384 - 3072, "range held to 12: %d", *slot(0, 17) - 16384);
    ALL(range, 12 * 256);
    words_reset(16384 + 13 * 256);                /* TUNE knob at +13, bend +12: TUNE's own top */
    pb_in(13, 16383, AUTO);
    me_render(words);
    CHECK(*slot(0, 17) == TUNE_MAX, "capped at TUNE +24: %d", *slot(0, 17));
    words_reset(16384 - 13 * 256);
    pb_in(13, 0, AUTO);
    me_render(words);
    CHECK(*slot(0, 17) == TUNE_MIN, "capped at TUNE -24: %d", *slot(0, 17));
    ALL(range, 0 * 256);
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
    me_audio_cfg[0].range = 12 * 256;
    me_audio_cfg[1].range = 0 * 256;
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
        const uint8_t *r = note_out(3, on);
        CHECK(r != on && r[0] == 0x94 && r[1] == 60 && r[2] == 64, "MIDI C's note on its channel: velocity 1 -> %d", r[2]);
        CHECK(on[2] == 1, "the caller's bytes untouched");
        CHECK(note_out(3, off) == off, "a note off passes as it is");
        CHECK(note_out(3, other) == other, "another channel passes as it is");
        const uint8_t two[6] = { 0x94, 60, 100, 0x94, 64, 2 };
        r = note_out(6, two);
        CHECK(r[2] == 114 && r[5] == 65, "two notes in one call: %d %d", r[2], r[5]);
        me_midi_cfg[2].vel_sens = 100;
        CHECK(note_out(3, on) == on, "100 %%: as played");
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
    CHECK(me_audio_cfg[0].vel_sens == 100 && me_audio_cfg[0].range == 2 * 256, "V.SNS 100 %%, BEND 2");
    CHECK(ENC(1, 3) == 1 && me_audio_cfg[0].range == 5 * 256, "A BEND 2 -> 5: %d", me_audio_cfg[0].range);
    CHECK(h_enc_last_sid == SID_DEPTH, "BEND turns as finely as a depth");
    for (int i = 0; i < 20; i++) ENC(1, 1);
    CHECK(me_audio_cfg[0].range == BEND_MAX, "BEND at most 12: %d", me_audio_cfg[0].range);
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
    CHECK(me_audio_cfg[1].range == 2 * 256 && me_audio_cfg[1].mw_amount == 0, "track 2 unchanged");
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
    CHECK(me_audio_cfg[0].range == 3 * 256, "BEND 2 -> 3: %d", me_audio_cfg[0].range);
    ENC(1, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].range == 4 * 256, "-> 4: every whole semitone: %d", me_audio_cfg[0].range);
    me_audio_cfg[0].range = 4 * 256 + 100;        // between whole semitones (4.39)
    ENC(1, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].range == 5 * 256, "4.39 up: 5: %d", me_audio_cfg[0].range);
    me_audio_cfg[0].range = 4 * 256 + 100;
    ENC(1, -1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].range == 4 * 256, "4.39 down: 4: %d", me_audio_cfg[0].range);
    me_audio_cfg[0].range = 256;
    ENC(1, -1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].range == 0, "1 down: OFF: %d", me_audio_cfg[0].range);
    me_audio_cfg[0].range = 12 * 256;
    ENC(1, 1); for (int i = 0; i < SNAP_REST; i++) me_page_tick(0);
    CHECK(me_audio_cfg[0].range == 12 * 256, "12 up: 12: %d", me_audio_cfg[0].range);
    me_audio_cfg[0].range = 2 * 256;              // fine: the words as they are, as a depth
    h_func_held = 0; h_enc_fine = 1; ENC(1, 1); h_enc_fine = 0; h_func_held = 1;
    CHECK(me_audio_cfg[0].range == 2 * 256 + 4, "BEND fine: 2 + 4/256 (as a depth): %d", me_audio_cfg[0].range);
    me_audio_cfg[0].range = 12 * 256;
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
    CHECK(me_midi_cfg[0].mw_dest == MIDI_DESTS - 1, "to the end: LFO DEP: %d", me_midi_cfg[0].mw_dest);
    memset(drawn, 0, sizeof drawn);
    me_page_draw((void *)1, (void *)1);
    CHECK(strstr(drawn, "LFO:") && strstr(drawn, "DEP"), "the LFO's lines and box: %s", drawn);
    ENC(3, -5);
    CHECK(me_midi_cfg[0].mw_dest == 12, "back to CC VAL8: %d", me_midi_cfg[0].mw_dest);
    SEL(0, 7, 74);
    memset(drawn, 0, sizeof drawn);
    me_page_draw((void *)1, (void *)1);
    CHECK(strstr(drawn, "CC:CC8 VAL #74") && strstr(drawn, "#74") && strstr(drawn, "VAL8"), "the list and box show SEL 74: %s", drawn);
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
    note_out(3, note);
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
                case 6: { uint8_t n[3] = { 0x90, (uint8_t)RND(128), (uint8_t)(1 + RND(127)) }; const uint8_t *r2 = note_out(3, n);
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

    // 40. Kept with the project (store.c): text files on the +Drive.
    {
#define TICKS(n) for (int i_ = 0; i_ < (n); i_++) me_tick(0)
        reset();
        h_nfs = 0; h_mounted = 0; booted = 0; dirs_made = 0; cur_slot = -1; current_dirty = 0; job.kind = 0;
        def_preset[0] = def_preset[1] = -1; pscan[0] = pscan[1] = 0;
        me_tick(0);
        CHECK(!booted, "drive not mounted: waits");
        h_mounted = 1; h_owner = 1; me_tick(0);
        CHECK(!booted, "drive busy: waits");
        h_owner = 0; me_tick(0);
        CHECK(booted && me_midi_cfg[0].thru == 1 && me_audio_cfg[0].range == 2 * 256, "first power-on: nothing on the card, the factory settings");
        // a change: current.txt a second later, in _temp first
        me_midi_cfg[0].mw_dest = 7; me_midi_cfg[0].mw_amount = 20 * 256; me_audio_cfg[3].range = 7 * 256;
        TICKS(SAVE_WAIT);
        CHECK(!h_ftext("/cfw/expression/current.txt"), "not before a second");
        TICKS(12 + QUIET);                                // looked at three times a second, a second, and quiet
        const char *cur = h_ftext("/cfw/expression/current.txt");
        CHECK(cur && h_ftext("/cfw/expression/_temp/current.txt"), "then written, with its copy");
        CHECK(cur && strstr(cur, "[A]") && strstr(cur, "MW=7") && strstr(cur, "M.DEP=+20.00") && strstr(cur, "BEND=7.00") && strstr(cur, "\nEND\n"), "readable text: %s", cur ? cur : "-");
        CHECK(cur && h_fs[h_find("/cfw/expression/current.txt")].len == PROJ_SIZE, "a fixed size");
        // SAVE PROJECT AS slot 003 "SONG A"
        me_project_saving("SONG A", 2);
        me_midi_cfg[0].mw_dest = 9;                       // a change during the save is not in it
        TICKS(1);
        const char *pj = h_ftext("/cfw/expression/projects/003.txt");
        CHECK(pj && strstr(pj, "NAME=SONG A") && strstr(pj, "SLOT=3") && strstr(pj, "MW=7"), "projects/003.txt, as at the save: %s", pj ? pj : "-");
        CHECK(h_ftext("/cfw/expression/_temp/projects/003.txt") != 0, "and its copy in _temp");
        // edits, then LOAD PROJECT slot 003: as saved
        me_midi_cfg[0].mw_dest = 2; me_audio_cfg[3].range = 1 * 256;
        TICKS(40);
        me_project_loading("SONG A", 2);
        TICKS(1);
        CHECK(me_midi_cfg[0].mw_dest == 7 && me_audio_cfg[3].range == 7 * 256 && me_midi_cfg[0].mw_amount == 20 * 256, "loaded slot 003: as saved (%d %d)", me_midi_cfg[0].mw_dest, me_audio_cfg[3].range);
        // a renamed project: still slot 003
        me_project_loading("RENAMED", 2);
        TICKS(1);
        CHECK(me_midi_cfg[0].mw_dest == 7, "renamed: found by its slot");
        // a project without a file: the defaults
        me_project_loading("OTHER", 9);
        TICKS(1);
        CHECK(me_midi_cfg[0].mw_dest == 0 && me_audio_cfg[3].range == 2 * 256 && me_midi_cfg[0].thru == 1, "slot 010 without a file: the factory defaults");
        // power-on: as at power-off
        me_midi_cfg[5].vel_sens = 40;
        TICKS(40);
        booted = 0; me_midi_cfg[5].vel_sens = 100; me_audio_cfg[0].range = 9 * 256;
        TICKS(1);
        CHECK(me_midi_cfg[5].vel_sens == 40 && me_audio_cfg[0].range == 2 * 256 && cur_slot == 9, "power-on: as at power-off (%d), slot kept %d", me_midi_cfg[5].vel_sens, cur_slot);
        // a torn current.txt: the copy in _temp is read
        int ci = h_find("/cfw/expression/current.txt");
        h_fs[ci].len = 300;                               // cut short: no END
        booted = 0; me_midi_cfg[5].vel_sens = 100;
        u32 bad = n_bad;
        TICKS(1);
        CHECK(me_midi_cfg[5].vel_sens == 40 && n_bad == bad + 1, "a torn file: the _temp copy (%d)", me_midi_cfg[5].vel_sens);
        // both broken: the defaults, never garbage
        h_fs[ci].len = 300; h_fs[h_find("/cfw/expression/_temp/current.txt")].len = 10;
        booted = 0;
        TICKS(1);
        CHECK(me_midi_cfg[5].vel_sens == 100 && me_midi_cfg[0].thru == 1, "both broken: the defaults");
        // nothing is written while notes play
        me_midi_cfg[6].vel_sens = 33;
        { u32 w = n_written;
          for (int i = 0; i < 300; i++) { me_activity++; me_tick(0); }
          CHECK(n_written == w, "playing: current.txt waits (%u)", n_written - w);
          TICKS(QUIET + 2);
          CHECK(n_written == w + 1, "then written once quiet"); }
        me_midi_cfg[6].vel_sens = 100;
        TICKS(12 + SAVE_WAIT + QUIET);
        // a hand-edited project file: odd spacing, comments, CRLF, out of range, junk lines, missing keys
        h_put_text("/cfw/expression/projects/004.txt",
            "MIDI EXPRESSION PROJECT 1\r\nNAME=Hand made\r\n; my notes\r\n[1]\r\nBEND = 99\r\nV.SNS=-5\r\nMW=26 ; filter\r\n"
            "M.DEP=+12.5\r\nVEL=37\r\nV.DEP=-300\r\nnonsense line\r\n[b]\r\nTHRU=off\r\nAT=17\r\nA.DEP=-64.00\r\nMW=999\r\n[Z]\r\nBEND=3\r\nEND\r\n");
        me_project_loading("x", 3);
        TICKS(1);
        CHECK(me_audio_cfg[0].range == 12 * 256 && me_audio_cfg[0].vel_sens == 0 && me_audio_cfg[0].mw_dest == 26
              && me_audio_cfg[0].mw_amount == 32 * 100 && me_audio_cfg[0].vel_dest == 0 && me_audio_cfg[0].vel_amount == -AMOUNT_MAX,
              "hand edits: held in range (%d %d %d %d %d %d)", me_audio_cfg[0].range, me_audio_cfg[0].vel_sens, me_audio_cfg[0].mw_dest,
              me_audio_cfg[0].mw_amount, me_audio_cfg[0].vel_dest, me_audio_cfg[0].vel_amount);
        CHECK(me_midi_cfg[1].thru == 0 && me_midi_cfg[1].at_dest == 17 && me_midi_cfg[1].at_amount == -AMOUNT_MAX && me_midi_cfg[1].mw_dest == MIDI_DESTS - 1,
              "MIDI B from [b]: THRU off, AT -> LFO depth -64, MW held (%d)", me_midi_cfg[1].mw_dest);
        CHECK(me_audio_cfg[2].range == 2 * 256 && me_midi_cfg[0].thru == 1, "untouched tracks: factory");
        // presets: save, list, load, reset, default
        me_audio_cfg[0].range = 5 * 256; me_audio_cfg[0].mw_dest = 27; me_audio_cfg[0].mw_amount = 10 * 256;
        CHECK(me_preset_save(0, 4, "Lead  ", 0), "save audio preset 05");
        CHECK(!me_preset_save(0, 5, "x", 0), "one job at a time");
        TICKS(1);
        const char *pr = h_ftext("/cfw/expression/audio/05.txt");
        CHECK(pr && strstr(pr, "NAME=Lead\n") && strstr(pr, "BEND=5.00") && h_ftext("/cfw/expression/_temp/audio/05.txt"), "audio/05.txt: %s", pr ? pr : "-");
        me_presets_rescan(0);
        TICKS(10);
        CHECK(me_presets_ready(0) && !strcmp(me_preset_name(0, 4), "Lead") && !*me_preset_name(0, 0), "the list: 05 Lead, the rest empty");
        me_audio_cfg[6].range = 0 * 256;
        CHECK(me_preset_load(0, 4, 6), "load preset 05 on track 7");
        TICKS(1);
        CHECK(me_audio_cfg[6].range == 5 * 256 && me_audio_cfg[6].mw_dest == 27 && me_audio_cfg[6].mw_amount == 10 * 256, "track 7 is Lead");
        me_default_set(0, 4);
        TICKS(1);
        CHECK(h_ftext("/cfw/expression/config.txt") && strstr(h_ftext("/cfw/expression/config.txt"), "DEFAULT AUDIO=5"), "config: default audio 5");
        me_audio_cfg[2].range = 11 * 256;
        me_track_reset(0, 2);
        TICKS(1);
        CHECK(me_audio_cfg[2].range == 5 * 256 && me_audio_cfg[2].mw_dest == 27, "RESET: the default preset, Lead");
        me_project_loading("NEW", 20);
        TICKS(1);
        CHECK(me_audio_cfg[0].range == 5 * 256 && me_audio_cfg[7].mw_dest == 27 && me_midi_cfg[0].thru == 1, "a new project: Lead on every audio track, factory MIDI");
        def_preset[0] = 9; booted = 0;                     // config read back at power-on
        TICKS(1);
        CHECK(def_preset[0] == 4, "config read at power-on: %d", def_preset[0]);
        h_fs[h_find("/cfw/expression/audio/05.txt")].len = 5; h_fs[h_find("/cfw/expression/_temp/audio/05.txt")].len = 5;
        me_track_reset(0, 1);
        TICKS(1);
        CHECK(me_audio_cfg[1].range == 2 * 256, "the default preset broken: factory");
        me_default_set(0, -1);
        TICKS(1);
        CHECK(strstr(h_ftext("/cfw/expression/config.txt"), "DEFAULT AUDIO=0"), "default back to Factory");
        // a write that fails: counted, tried again
        h_write_fail = 2;
        me_midi_cfg[1].thru = 1;
        u32 fails = n_failed;
        TICKS(SAVE_WAIT + 12 + QUIET);
        CHECK(n_failed == fails + 1 && current_dirty, "a failed write: counted, still to save");
        h_write_fail = 0;
        TICKS(RETRY + 12);
        CHECK(!current_dirty, "saved on the retry");
        CHECK(h_open_count == 0 && h_max_open <= 1, "every file closed; never two open (%d %d)", h_open_count, h_max_open);
        // fuzz: random bytes as files never crash or put a value out of range
        for (int r = 0; r < 3000; r++) {
            char junk[600]; int n = 30 + RND(500);
            const char *heads[] = { "MIDI EXPRESSION PROJECT 1\n", "MIDI EXPRESSION PRESET AUDIO 1\n", "" };
            int hl = (int)strlen(heads[r % 3]); memcpy(junk, heads[r % 3], (size_t)hl);
            for (int i = hl; i < n; i++) { int c = RND(10); junk[i] = (char)(c < 3 ? "=\n[]+-.;END"[RND(11)] : c < 6 ? '0' + RND(10) : c < 8 ? "BENDMWVATHRUSNSDEPLIX"[RND(21)] : RND(256)); }
            if (RND(2)) memcpy(junk + n - 4, "END\n", 4);
            junk[n] = 0;
            h_put_text("/cfw/expression/projects/050.txt", junk);
            h_put_text("/cfw/expression/_temp/projects/050.txt", junk);
            me_project_loading("f", 49);
            TICKS(1);
            for (int t = 0; t < 8; t++) {
                CHECK(me_audio_cfg[t].range >= 0 && me_audio_cfg[t].range <= BEND_MAX && me_dest_valid(0, me_audio_cfg[t].mw_dest) && me_dest_valid(0, me_audio_cfg[t].vel_dest)
                      && me_dest_valid(0, me_audio_cfg[t].at_dest) && me_audio_cfg[t].vel_sens >= 0 && me_audio_cfg[t].vel_sens <= 100
                      && me_audio_cfg[t].mw_amount >= -AMOUNT_MAX && me_audio_cfg[t].mw_amount <= AMOUNT_MAX, "fuzz %d: audio %d in range", r, t);
                CHECK(me_midi_cfg[t].thru >= 0 && me_midi_cfg[t].thru <= 1 && me_midi_cfg[t].mw_dest >= 0 && me_midi_cfg[t].mw_dest < MIDI_DESTS
                      && me_midi_cfg[t].at_amount >= -AMOUNT_MAX && me_midi_cfg[t].at_amount <= AMOUNT_MAX, "fuzz %d: MIDI %d in range", r, t);
            }
        }
        // the page's menu: a 1 s push of knob A
        reset(); OPEN();
        KEY(40, 1, 0); h_push_held = 1;
        for (int i = 0; i < LONG_PUSH + 1; i++) me_page_tick(0);
        CHECK(me_menu == M_MAIN, "a 1 s push of A: the menu");
        h_push_held = 0; KEY(40, 0, 0);
        KEY(K_DOWN, 1, 0); KEY(K_YES, 1, 0);                 // SAVE
        CHECK(me_menu == M_SAVE, "SAVE: the slots");
        TICKS(10);
        KEY(K_DOWN, 1, 0); KEY(K_YES, 1, 0);                 // slot 02
        CHECK(me_menu == M_NAME && !strcmp(edit_name, "NEW"), "the name: NEW (%s)", edit_name);
        KEY(K_UP, 1, 0);                                     // N -> O
        KEY(K_RIGHT, 1, 0); KEY(K_RIGHT, 1, 0); KEY(K_RIGHT, 1, 0); KEY(K_UP, 1, 0);   // past the end: a new character
        KEY(K_YES, 1, 0);
        TICKS(1);
        CHECK(!strcmp(me_preset_name(1, 1), "OEWA") && h_ftext("/cfw/expression/midi/02.txt"), "MIDI track A: saved as midi/02 OEWA: %s", me_preset_name(1, 1));
        CHECK(me_menu == M_MAIN, "back to the menu");
        KEY(K_NO, 1, 0);
        CHECK(me_menu == M_OFF && me_page_open, "NO: back to the page");
        // the longest a project file gets: every destination, negative depths: within PROJ_SIZE with room
        for (int t = 0; t < 8; t++) {
            me_audio_cfg[t].mw_dest = 44; me_audio_cfg[t].vel_dest = 39; me_audio_cfg[t].at_dest = 43; me_audio_cfg[t].range = 12 * 256; me_audio_cfg[t].vel_sens = 100;
            me_audio_cfg[t].mw_amount = me_audio_cfg[t].vel_amount = me_audio_cfg[t].at_amount = -AMOUNT_MAX;
            me_midi_cfg[t].mw_dest = 12; me_midi_cfg[t].vel_dest = 11; me_midi_cfg[t].at_dest = 10;
            me_midi_cfg[t].mw_amount = me_midi_cfg[t].vel_amount = me_midi_cfg[t].at_amount = -AMOUNT_MAX;
            SEL(t, 7, 127); SEL(t, 6, 127); SEL(t, 5, 127);
        }
        take_snapshot();
        {
            int n = write_project(buf, "ABCDEFGHIJKL", 127), used = (int)(strstr(buf, "\nEND\n") - buf) + 5;
            CHECK(n == PROJ_SIZE && used < PROJ_SIZE - 600, "the longest project file: %d of %d bytes", used, PROJ_SIZE);
        }
        // LOAD, RESET and DEFAULT through the menu, with the LEVEL knob
        reset(); OPEN();
        h_active_track = 0;                                  // audio track 1
        me_audio_cfg[0].range = 3 * 256;
        me_menu = M_MAIN; menu_cur[M_MAIN] = 0;
        KEY(K_YES, 1, 0);                                    // LOAD
        TICKS(10);
        CHECK(me_menu == M_LOAD && me_presets_ready(0), "LOAD: the audio presets");
        { int ev2[8] = { 0, 0, 0, 9, RAW_DETENT - 1, 0, 0, 0 }; me_page_enc(0, ev2); }
        CHECK(menu_cur[M_LOAD] == 0, "LEVEL: less than a detent, no step");
        for (int i = 0; i < 4 * RAW_DETENT - (RAW_DETENT - 1); i++) { int ev2[8] = { 0, 0, 0, 9, 1, 0, 0, 0 }; me_page_enc(0, ev2); }   // a count an event, as a unit sends
        CHECK(menu_cur[M_LOAD] == 4, "LEVEL moves the cursor: %d", menu_cur[M_LOAD]);
        KEY(K_YES, 1, 0);
        TICKS(1);
        CHECK(!strcmp(me_preset_name(0, 4), "Lead") ? me_audio_cfg[0].range == 5 * 256 : 1, "slot 05 loaded on track 1 (%d)", me_audio_cfg[0].range);
        me_menu = M_MAIN; menu_cur[M_MAIN] = 2;
        KEY(K_YES, 1, 0);                                    // RESET
        CHECK(me_menu == M_RESET, "RESET asks first");
        KEY(K_YES, 1, 0);
        TICKS(1);
        CHECK(me_audio_cfg[0].range == 2 * 256 && me_menu == M_MAIN, "RESET: the default (Factory) on track 1");
        menu_cur[M_MAIN] = 3;
        KEY(K_YES, 1, 0);                                    // DEFAULT
        TICKS(10);
        CHECK(me_menu == M_DEFAULT && menu_cur[M_DEFAULT] == 0, "DEFAULT: the cursor on the current one, FACTORY");
        KEY(K_NO, 1, 0); KEY(K_NO, 1, 0);
        CHECK(me_menu == M_OFF, "NO, NO: back to the page");
        // keys in the menu: FUNC's release reaches stock; a knob push is YES; another page key leaves
        reset(); OPEN(); h_active_track = 0;
        me_menu = M_MAIN; menu_cur[M_MAIN] = 0;
        CHECK(KEY(1, 1, 0) == 0 && KEY(1, 0, 0) == 0, "FUNC down and up both go to stock (no stuck FUNC)");
        CHECK(KEY(28, 1, 0) == 0 && KEY(28, 0, 0) == 0, "a trig key and its release play as stock");
        KEY(43, 1, 0); KEY(43, 0, 0);                        // push of knob D: as YES
        CHECK(me_menu == M_LOAD, "a knob push goes into LOAD");
        CHECK(KEY(21, 1, 0) == 0 && !me_page_open && me_menu == M_OFF, "FLTR: the page and its menu go, stock opens FLTR");
        // the name: FUNC held shows the letters, arrows pick, letting go writes it
        reset(); OPEN(); h_active_track = 0;
        me_menu = M_NAME; edit_slot = 3; strcpy(edit_name, "AB"); edit_pos = 1;
        h_func_held = 1; KEY(1, 1, 0);
        CHECK(grid_on && grid_cur == 1, "FUNC: the grid, on B");
        KEY(K_RIGHT, 1, 0); KEY(K_DOWN, 1, 0);                // C, then one row down: R
        h_func_held = 0; KEY(1, 0, 0);
        CHECK(!strcmp(edit_name, "AR") && edit_pos == 2, "let go: R written, on to the next place (%s %d)", edit_name, edit_pos);
        strcpy(edit_name, ""); edit_pos = 0;                 // FAST: FUNC, pick, let go, four times
        { const char *w = "FAST";
          for (int i = 0; w[i]; i++) {
              h_func_held = 1; KEY(1, 1, 0);
              while (grid_chars[grid_cur] != w[i]) KEY(K_RIGHT, 1, 0);
              h_func_held = 0; KEY(1, 0, 0);
          } }
        CHECK(!strcmp(edit_name, "FAST") && edit_pos == 4, "FUNC, pick, let go x4: FAST (%s %d)", edit_name, edit_pos);
        strcpy(edit_name, "ABCDEFGHIJKL"); edit_pos = ME_NAME_LEN - 1;
        h_func_held = 1; KEY(1, 1, 0); h_func_held = 0; KEY(1, 0, 0);
        CHECK(edit_pos == ME_NAME_LEN - 1 && edit_name[ME_NAME_LEN] == 0, "the last place: the cursor stays (%d)", edit_pos);
        // a twist of A while held: a nudge keeps the menu coming, a real turn cancels it
        reset(); OPEN();
        KEY(40, 1, 0); h_push_held = 1;
        { int e1[8] = { 0, 0, 0, 1, RAW_DETENT, 0, 0, 0 }; me_page_enc(0, e1); }
        for (int i = 0; i < LONG_PUSH + 1; i++) me_page_tick(0);
        CHECK(me_menu == M_MAIN, "a nudge while held: the menu still opens");
        h_push_held = 0; KEY(40, 0, 0); me_menu = M_OFF;
        KEY(40, 1, 0); h_push_held = 1;
        { int e1[8] = { 0, 0, 0, 1, RAW_DETENT * 5, 0, 0, 0 }; me_page_enc(0, e1); }
        for (int i = 0; i < LONG_PUSH + 1; i++) me_page_tick(0);
        CHECK(me_menu == M_OFF, "a real turn while held: no menu (the push multiplier)");
        h_push_held = 0; KEY(40, 0, 0);
        // a plain push of A (shorter than a second) does not open it
        KEY(40, 1, 0); h_push_held = 1;
        for (int i = 0; i < 10; i++) me_page_tick(0);
        h_push_held = 0; KEY(40, 0, 0);
        for (int i = 0; i < 40; i++) me_page_tick(0);
        CHECK(me_menu == M_OFF, "a short push: no menu");
        reset();
#undef TICKS
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
