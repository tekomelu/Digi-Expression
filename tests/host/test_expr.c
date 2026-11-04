// Host unit tests for MIDI Expression's logic (module/midi-expression/src/expr.c).
//   cc -std=c11 -Wall -DME_HOST_TEST -I tests/host tests/host/test_expr.c -o build/test_expr && build/test_expr
#include <stdio.h>
#include <string.h>

#include "../../module/midi-expression/src/expr.c"
#include "../../module/midi-expression/src/audio.c"

volatile int32_t h_auto_channel = 13, h_active_track = 8, h_track_channel[16], h_receive_cc = 1;
volatile uint32_t h_output_to = 3;
volatile int32_t h_tx_pending;
volatile uint32_t h_tx_base, h_tx_sent;
volatile uint16_t h_dma_a, h_dma_b;

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

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void reset(void)
{
    memset((void *)chans, 0, sizeof chans);
    initialised = 0; flushing = 0; waiting = 0; last_active = -1;
    me_pb_on = me_at_on = me_mw_on = 1;
    for (int t = 0; t < 16; t++) h_track_channel[t] = t;   // tracks 1-8 ch 1-8, A-H ch 9-16
    for (int m = 0; m < 8; m++) chan_of[m] = -1;
    chan_of[0] = 0;                                        // MIDI track A: CHAN on, channel 1
    h_active_track = 8; h_auto_channel = 13;
    h_tx_pending = 0; h_tx_base = h_tx_sent = 0; h_dma_a = h_dma_b = 0;
    nsent = 0; stock_cc_calls = 0;
    memset((void *)atracks, 0, sizeof atracks);
    me_bend_range = 2; me_mw_dest = 0; me_at_dest = 0; me_mw_amount = 32; me_at_amount = 32;
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

static int pb_of(int i) { return sent[i][1] | (sent[i][2] << 7); }

static void pb_in(int ch, int value, int flag) { uint8_t m[3] = { (uint8_t)(0xE0 | ch), (uint8_t)(value & 127), (uint8_t)(value >> 7) }; me_pitch_bend(m, 3, flag); }
static void at_in(int ch, int v, int flag) { uint8_t m[2] = { (uint8_t)(0xD0 | ch), (uint8_t)v }; me_aftertouch(m, 2, flag); }
static void mw_in(int ch, int v, int flag) { uint8_t m[3] = { (uint8_t)(0xB0 | ch), 1, (uint8_t)v }; me_control_change(m, 3, flag); }
// What the MIDI tracks' task would hand stock send(): PB 14-bit, AT/MW 8.8 fixed.
static int32_t track_pb(int ch, int v) { return me_filter(0, ch, ID_PB, v, 0); }
static int32_t track_at(int ch, int v) { return me_filter(0, ch, ID_AT, v << 8, 0); }
static int32_t track_mw(int ch, int v) { return me_filter(0, ch, ID_MW, v << 8, 0); }

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

    // 8. Unticking a row gives the track its value back.
    reset();
    track_pb(0, 8192);
    pb_in(13, 14000, AUTO);
    me_pb_on = 0;
    before = nsent;
    me_tick(0);
    CHECK(nsent == before + 1 && pb_of(nsent - 1) == 8192, "unticked: PB back to the track's: %d", nsent > before ? pb_of(nsent - 1) : -1);

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
    CHECK(me_filter(0, 0, 24, 77 << 8, 0) == (77 << 8), "breath (id 24) untouched");
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
    me_bend_range = 12;
    words_reset(16384);
    pb_in(13, 16383, AUTO);                       // full up: 8191/8192 of 12 semitones
    me_render(words);
    CHECK(*slot(0, 17) == 16384 + 3071, "TUNE +12: %d", *slot(0, 17) - 16384);
    words_reset(16384);
    pb_in(13, 0, AUTO);
    me_render(words);
    CHECK(*slot(0, 17) == 16384 - 3072, "TUNE -12: %d", *slot(0, 17) - 16384);
    me_bend_range = 24;                           // more than the row allows: held to 12
    words_reset(16384);
    pb_in(13, 0, AUTO);
    me_render(words);
    CHECK(*slot(0, 17) == 16384 - 3072, "range held to 12: %d", *slot(0, 17) - 16384);
    me_bend_range = 12;
    words_reset(16384 + 13 * 256);                /* TUNE knob at +13, bend +12: TUNE's own top */
    pb_in(13, 16383, AUTO);
    me_render(words);
    CHECK(*slot(0, 17) == TUNE_MAX, "capped at TUNE +24: %d", *slot(0, 17));
    words_reset(16384 - 13 * 256);
    pb_in(13, 0, AUTO);
    me_render(words);
    CHECK(*slot(0, 17) == TUNE_MIN, "capped at TUNE -24: %d", *slot(0, 17));
    me_bend_range = 0;
    words_reset(16384);
    me_render(words);
    CHECK(*slot(0, 17) == 16384, "PB RANGE OFF: TUNE untouched");

    // 14. Mod wheel and aftertouch routes add in the stock LFO's scale, within 0..32512.
    reset();
    h_active_track = 2;                           // audio track 3
    me_mw_dest = 26; me_mw_amount = 64;           // FLT.FREQ, full
    me_at_dest = 8; me_at_amount = -32;           // L1.DEP, half, down
    words_reset(20000);
    mw_in(13, 127, AUTO);
    at_in(13, 100, AUTO);
    me_render(words);
    CHECK(*slot(2, 26) == PARAM_MAX, "MW full to FLT.FREQ: capped at the top: %d", *slot(2, 26));
    CHECK(*slot(2, 8) == 20000 - 100 * 32 * 4, "AT 100 x -32 on L1.DEP: %d", *slot(2, 8));
    words_reset(1000);
    me_render(words);
    CHECK(*slot(2, 8) == 0, "capped at 0: %d", *slot(2, 8));
    words_reset(10000);
    mw_in(13, 64, AUTO);
    me_mw_amount = 32;
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
    CHECK(*slot(2, 26) == 10000 && *slot(2, 8) == 10000, "released on track change: %d %d", *slot(2, 26), *slot(2, 8));
    CHECK(*slot(3, 26) == 10000, "the new track only follows the next move");
    mw_in(13, 32, AUTO);
    me_render(words);
    CHECK(*slot(3, 26) == 10000 + 32 * 32 * 4, "new track after a move: %d", *slot(3, 26) - 10000);

    // 16. A TRACK CHANNEL reaches its audio track whatever is selected, and is kept on a track change.
    reset();
    me_mw_dest = 27;                              // FLT.RESO
    words_reset(5000);
    mw_in(4, 100, TRACK);                         // channel 5: audio track 5
    h_active_track = 1;
    me_tick(0);
    me_render(words);
    CHECK(*slot(4, 27) == 5000 + 100 * 32 * 4, "track-channel route kept: %d", *slot(4, 27) - 5000);

    // 17. Unticking a row stops it on audio tracks too.
    me_mw_on = 0;
    me_tick(0);
    words_reset(5000);
    me_render(words);
    CHECK(*slot(4, 27) == 5000, "MW THRU unticked: no route: %d", *slot(4, 27));
    me_mw_on = 1;

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
