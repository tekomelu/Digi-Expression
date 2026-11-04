// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI Expression: the EXPRESSION page, the TRIG page's second page. Each
// track's own settings, laid out like the stock FLTR and LFO pages: eight
// cells on knobs A-H, under the TRIG page's own header and track box.
//
//   audio track  A BEND   B V.SNS  C MW    D M.DEP
//                E VEL    F V.DEP  G AT    H A.DEP
//   MIDI track   A PB     B AT     C MW    D V.SNS
//                E -      F -      G MW>   H AT>
//
//   BEND   OFF, 1..12 semitones of TUNE (default 2)
//   V.SNS  0..100 %: how much the played velocity counts (default 100 %)
//   MW, VEL, AT        the parameter each source moves (default none);
//                      turning opens the list, as the LFO page's DEST does,
//                      and pushing that knob closes it
//   M.DEP, V.DEP, A.DEP  -64.00..+64.00, how far (default 0), as finely as
//                      the LFO page's DEP
//   PB, AT, MW         a MIDI track passes them or not (default ON)
//   MW>    CC 1 (adds to the track's own MW, default) or CC 2..119 alone
//   AT>    channel aftertouch (default) or CC 1..119 alone
//
// Opening it. On the TRIG page, a tap of TRIG (stock: nothing; a hold stays
// the stock page's, showing its values, with TRIG + REC/STOP/PLAY). FUNC +
// TRIG stays the stock quantize page. Leaving: TRIG or NO, or any other page
// or menu key, which then does its stock job. The trig keys, TRK + trig,
// FUNC, PTN, BANK, the transport and the LEVEL knob keep their stock jobs.
//
// Knobs. A turn goes through the firmware's own encoder accelerator with the
// step size of a stock parameter of the same kind (as Digi Perform does), so
// the page feels like the stock pages; pushed, the stock push multiplier.
// FUNC + turn jumps between useful values, as FUNC + TUNE jumps octaves:
// depths -64, -48 .. +48, +64; BEND OFF, 1, 2, 3, 5, 7, 12; V.SNS 0, 25, 50,
// 75, 100 %; a destination to the next page's first (SRC, FLTR, AMP, LFO1,
// LFO2); a CC to the next ten. After each jump the knob rests 0.4 s, as the
// firmware does.
//
// Which page is up. The UI's view controller is the brain + 0x40 (the ctrl
// that ev_tick and ev_draw get). Measured in digiemu on OS 1.54 (stock and
// with this mod): its +0x14 is a list of layers, nodes {next, prev, view};
// the first layer's view is the main frame (vtable 0x40183268), and +0x1c
// is the layer that has focus, the first one unless a menu or popup (such
// as SETTINGS or the scale setup) is over the page. The frame keeps the page
// it shows at +0xb4. The TRIG page's view (audio and MIDI tracks alike) has
// the vtable 0x40184308 and keeps its key, 19 (TRIG), at +0x88. All of it
// only read, never written.
//
// Held in RAM: every setting goes back to its default at power-on.

#include "expr.h"

#define D_RANGE   2
#define D_AMOUNT  0
#define D_SENS    100

struct me_audio_cfg me_audio_cfg[8] = {
#define A0 { D_RANGE, 0, 0, 0, D_SENS, D_AMOUNT, D_AMOUNT, D_AMOUNT }
    A0, A0, A0, A0, A0, A0, A0, A0,
#undef A0
};

struct me_midi_cfg me_midi_cfg[8] = {
#define M0 { 1, 1, 1, 1, 0, D_SENS }
    M0, M0, M0, M0, M0, M0, M0, M0,
#undef M0
};

#ifdef ME_HOST_TEST
#include "host_page.h"
#else
#define ACTIVE_TRACK_P (*(volatile int32_t *)0x4197c6b4)
#define TEXT        ((void (*)(void *, const void *, int, int, int, const char *, ...))0x400c27a4)
#define FILLRECT    ((void (*)(void *, int, int, int, int, int))0x400c1bce)
#define FONT5       ((const void *)0x40200ebc)
#define LABEL_FONT  ((const void *)0x401fe44c)  // the stock pages' labels, about 4 px a character
#define TITLE_FONT  ((const void *)0x401fd57c)  // the header's and the lists' font, 4 x 6, lower case
#define KEYHELD(k)  (((uint32_t (*)(int))0x400d399c)(k))
// The stock round knob (the SRC page's parameter object method, as Digi
// Perform documents it): (obj, id, value 8.8 0..0x7f00, 0, 0, bmp, x, y);
// id 148 is the plain knob. It spans x + 1..x + 17, y..y + 16.
#define KNOB_GFX    ((void (*)(void *, int, int, int, int, void *, int, int))0x4000f2bc)
// The firmware's name for a parameter id ("Frequency", ...).
#define STOCK_NAME  ((const char *(*)(int, int))0x4000feac)
// The firmware's encoder accelerator (as Digi Perform documents it): a
// parameter id's UI record, and accelerate(state, encoder, raw x multiplier,
// the record's curve, pushed) -> the change in 8.8 words; the state decays
// ten times a second.
#define ENC_DESC    ((const int *(*)(int))0x40065794)
#define ENC_ACC     ((int (*)(void *, int, int, const void *, int))0x400c06ae)
#define ENC_DECAY   ((void (*)(void *, void *))0x400c05f4)
#define TRIG_VTABLE  0x40184308u
#define FRAME_VTABLE 0x40183268u

static int ram(uint32_t a)
{
    return a >= 0x40000000u && a < 0x48000000u && !(a & 1);
}

// The TRIG page's view when it is the page shown and has focus, else 0.
static void *trig_page(void *ctrl)
{
    if (!ctrl)
        return 0;
    uint32_t first = *(volatile uint32_t *)((char *)ctrl + 0x14);
    uint32_t focus = *(volatile uint32_t *)((char *)ctrl + 0x1c);
    if (!ram(first) || first != focus)
        return 0;                       // a menu or popup over the page
    uint32_t frame = *(volatile uint32_t *)(first + 8);
    if (!ram(frame) || *(volatile uint32_t *)frame != FRAME_VTABLE)
        return 0;
    uint32_t view = *(volatile uint32_t *)(frame + 0xb4);
    if (!ram(view) || *(volatile uint32_t *)view != TRIG_VTABLE)
        return 0;
    if (*(volatile int32_t *)(view + 0x88) != 19)
        return 0;
    return (void *)view;
}

static int enc_state[64];               // the accelerator's state, 0x14 bytes an encoder from 0x20

static void enc_reset(void)
{
    for (int i = 0; i < 64; i++)
        enc_state[i] = 0;
    for (int i = 0; i < 10; i++)
        enc_state[(0x20 + 0x14 * i + 0x10) / 4] = 0x30;   // as the firmware clears it
}

// A turn of n raw counts on knob k (0..7) with the feel of stock parameter
// id sid: the change in 8.8 words (256 a step).
static int enc_words(int k, int sid, int n, int pushed)
{
    const int *d = ENC_DESC(sid);
    char curve[28];
    const char *src = (const char *)d[4];
    for (int i = 0; i < 28; i++)
        curve[i] = src[i];
    return ENC_ACC(enc_state, k, n * (pushed ? d[2] : d[1]), curve, pushed);
}

static void enc_decay(void)
{
    ENC_DECAY(enc_state, 0);
}
#endif

#define KEYID(ev)    (*(int *)((char *)(ev) + 12))
#define KEYDOWN(ev)  (*(int *)((char *)(ev) + 16) & 1)
#define KEYPRESS(ev) ((*(int *)((char *)(ev) + 16) & 9) == 1)   // down, not a repeat

enum {
    K_FUNC = 1, K_TRK = 2, K_YES = 12, K_NO = 13, K_UP = 14, K_DOWN = 15, K_LEFT = 16, K_RIGHT = 17,
    K_TRIG = 19, K_PUSH_A = 40,
};
#define TAP_TICKS  15                   // half a second at 30 Hz: shorter is a tap
#define SHOW_TICKS 30                   // a turned cell shows its value for a second
#define LIST_TICKS 60                   // the destination list stays two seconds after the last turn
#define CELLS      8

// Stock parameter ids whose knob feel each kind of cell borrows.
#define SID_TUNE   108                  // SRC TUNE: one step a detent when slow
#define SID_VOL    51                   // AMP VOL: 0..127
#define SID_DEPTH  59                   // LFO1 DEP: -64..+63
#define SID_LIST   111                  // the SAMP selector: a list, as Digi Perform's choosers

int me_page_open;                       // read by the tests
static int pend;                        // TRIG went down on the TRIG page
static int hold;                        // UI ticks since
static void *ctrl_seen;                 // the view controller, from ev_tick / ev_draw
static int recompose;                   // the page just closed: one more frame for the stock page
static int ticks;
static uint8_t shown[CELLS];            // ticks left showing the value instead of the label
static int list_cell = -1, list_t;      // the destination list: its cell, ticks left
static int acc[CELLS];                  // words turned that have not made a whole step yet
static int acc_track = -1;
static int idle_counts[CELLS];
static uint8_t snap_rest[CELLS];        // UI ticks a knob rests after a FUNC jump
#define SNAP_REST 12                    // 0.4 s, as the firmware locks a knob after a snap          // raw counts the accelerator has turned into nothing
#define RAW_DETENT 4                    // raw counts a detent (measured in digiemu)
#define IDLE_LIMIT (6 * RAW_DETENT)

static void close_page(void)
{
    if (me_page_open)
        recompose = 1;
    me_page_open = 0;
    list_cell = -1;
}

// ---- destinations ------------------------------------------------------------
// The sound's slots a source can move, in the order the list steps, as the
// LFO page lists them: SRC, FILTER, AMP, then the LFOs. Left out: the
// choices (PLAY, SAMP, LOOP, FLT TYPE, LFO DEST/WAVE/MODE) and slot 37,
// which the render never reads (scripts/probe-reads.py).
static const int8_t dest_order[] = {
    0,
    17, 19, 21, 22, 24,                         /* SRC: TUNE, BITR, STRT, LEN, LEV */
    26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, /* FLTR: FREQ .. REL, page 2 */
    38, 39, 40, 41, 42, 43, 44, 45,             /* AMP */
    1, 2, 3, 6, 8,                              /* LFO1: SPD, MULT, FADE, PHAS, DEP */
    9, 10, 11, 14, 16,                          /* LFO2 */
};
#define DEST_COUNT ((int)(sizeof dest_order / sizeof dest_order[0]))

// Short names, for the cell's box.
static const char *dest_short(int s)
{
    static const char *const n[46] = {
        "---",
        "SPD", "MULT", "FADE", "", "", "PHAS", "", "DEP",
        "SPD", "MULT", "FADE", "", "", "PHAS", "", "DEP",
        "TUNE", "", "BITR", "", "STRT", "LEN", "", "LEV",
        "", "FREQ", "RESO", "ENV", "ATK", "DEC", "SUS", "REL",
        "BASE", "WDTH", "DEL", "SRR", "",
        "ATK", "HOLD", "DEC", "OVER", "DEL", "REV", "PAN", "VOL",
    };
    return s >= 0 && s <= 45 ? n[s] : "---";
}

// The page the slot is on, for the box's first line and the list.
static const char *dest_page(int s)
{
    return s <= 0 ? "" : s <= 8 ? "LFO1" : s <= 16 ? "LFO2" : s <= 24 ? "SRC" : s <= 37 ? "FLTR" : "AMP";
}

// Which page a slot is on: 0 none, 1 SRC, 2 FLTR, 3 AMP, 4 LFO1, 5 LFO2.
static int dest_group(int s)
{
    return s <= 0 ? 0 : s <= 8 ? 4 : s <= 16 ? 5 : s <= 24 ? 1 : s <= 37 ? 2 : 3;
}

// The firmware's parameter id of a slot (Digi Perform's measured map).
static int slot_id(int s)
{
    if (s <= 8)
        return s + 51;
    if (s <= 16)
        return s + 52;
    if (s <= 24)
        return s + 91;
    if (s >= 29 && s <= 32)
        return s + 5;
    if (s <= 28)
        return s == 25 ? 33 : s == 26 ? 30 : s == 27 ? 32 : 38;
    if (s <= 35)
        return s == 33 ? 40 : s == 34 ? 41 : 39;
    return s + 6;
}

static int dest_index(int slot)
{
    for (int i = 0; i < DEST_COUNT; i++)
        if (dest_order[i] == slot)
            return i;
    return 0;
}

// ---- the cells ------------------------------------------------------------------
enum kind { C_NONE, C_RANGE, C_SENS, C_DEST, C_AMOUNT, C_ONOFF, C_MWCC, C_ATCC };

struct cell {
    uint8_t kind;
    const char *label;
};

static const struct cell audio_cells[CELLS] = {
    { C_RANGE, "BEND" }, { C_SENS, "V.SNS" }, { C_DEST, "MW" },  { C_AMOUNT, "M.DEP" },
    { C_DEST, "VEL" },   { C_AMOUNT, "V.DEP" }, { C_DEST, "AT" }, { C_AMOUNT, "A.DEP" },
};

static const struct cell midi_cells[CELLS] = {
    { C_ONOFF, "PB" }, { C_ONOFF, "AT" }, { C_ONOFF, "MW" }, { C_SENS, "V.SNS" },
    { C_NONE, "E" },   { C_NONE, "F" },   { C_MWCC, "MW>" }, { C_ATCC, "AT>" },
};

static int track(void)
{
    int t = ACTIVE_TRACK_P;
    return t >= 0 && t < 16 ? t : 0;
}

static const struct cell *cell_of(int t, int k)
{
    return t < 8 ? &audio_cells[k] : &midi_cells[k];
}

static int within(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

// The setting behind cell k of track t.
int me_page_value(int t, int k)
{
    if (t < 8) {
        struct me_audio_cfg *a = &me_audio_cfg[t];
        switch (k) {
        case 0: return a->range;
        case 1: return a->vel_sens;
        case 2: return a->mw_dest;
        case 3: return a->mw_amount;
        case 4: return a->vel_dest;
        case 5: return a->vel_amount;
        case 6: return a->at_dest;
        default: return a->at_amount;
        }
    }
    struct me_midi_cfg *m = &me_midi_cfg[t - 8];
    switch (k) {
    case 0: return m->pb_on;
    case 1: return m->at_on;
    case 2: return m->mw_on;
    case 3: return m->vel_sens;
    case 6: return m->mw_cc;
    case 7: return m->at_cc;
    default: return 0;
    }
}

int me_page_default(int t, int k)
{
    switch (cell_of(t, k)->kind) {
    case C_RANGE:  return D_RANGE;
    case C_SENS:   return D_SENS;
    case C_AMOUNT: return D_AMOUNT;
    case C_ONOFF:  return 1;
    case C_MWCC:   return 1;
    default:       return 0;            // C_DEST: none; C_ATCC: aftertouch
    }
}

static void set_value(int t, int k, int v)
{
    if (t < 8) {
        struct me_audio_cfg *a = &me_audio_cfg[t];
        switch (k) {
        case 0: a->range = (int8_t)v; break;
        case 1: a->vel_sens = (int8_t)v; break;
        case 2: a->mw_dest = (int8_t)v; break;
        case 3: a->mw_amount = (int16_t)v; break;
        case 4: a->vel_dest = (int8_t)v; break;
        case 5: a->vel_amount = (int16_t)v; break;
        case 6: a->at_dest = (int8_t)v; break;
        default: a->at_amount = (int16_t)v; break;
        }
        return;
    }
    struct me_midi_cfg *m = &me_midi_cfg[t - 8];
    int before = me_page_value(t, k);
    switch (k) {
    case 0: m->pb_on = (uint8_t)v; break;
    case 1: m->at_on = (uint8_t)v; break;
    case 2: m->mw_on = (uint8_t)v; break;
    case 3: m->vel_sens = (int8_t)v; break;
    case 6: m->mw_cc = (uint8_t)v; break;
    case 7: m->at_cc = (uint8_t)v; break;
    default: return;
    }
    if (k != 3 && before != v)
        me_midi_release_track(t - 8);   // the synth gets the track's own values back
}

static int sid_of(int kind)
{
    switch (kind) {
    case C_RANGE:  return SID_TUNE;
    case C_SENS:   return SID_VOL;
    case C_AMOUNT: return SID_DEPTH;
    default:       return SID_LIST;     // C_DEST, C_ONOFF, C_MWCC, C_ATCC
    }
}

// The next of a list of useful values from v, in the direction of d.
static int next_of(const int *list, int count, int v, int d)
{
    if (d > 0) {
        for (int i = 0; i < count; i++)
            if (list[i] > v)
                return list[i];
        return list[count - 1];
    }
    for (int i = count - 1; i >= 0; i--)
        if (list[i] < v)
            return list[i];
    return list[0];
}

// FUNC + turn: the next useful value of cell k from v, in the direction of d.
static int snap(int kind, int v, int d)
{
    static const int ranges[] = { 0, 1, 2, 3, 5, 7, 12 };
    static const int sens[] = { 0, 25, 50, 75, 100 };
    switch (kind) {
    case C_RANGE: return next_of(ranges, 7, v, d);
    case C_SENS:  return next_of(sens, 5, v, d);
    case C_AMOUNT: {
        int step = 16 * 256, q = v >= 0 ? v / step : -((-v + step - 1) / step);   // floor
        int lo = q * step;
        v = d > 0 ? lo + step : (lo == v ? v - step : lo);
        return within(v, -AMOUNT_MAX, AMOUNT_MAX);
    }
    case C_DEST: {                      // the first of the next page, or of this one going back
        int i = dest_index(v), g = dest_group(v);
        if (d > 0) {
            while (i + 1 < DEST_COUNT && dest_group(dest_order[i]) == g)
                i++;
        } else if (i > 0) {
            if (dest_group(dest_order[i - 1]) != g)
                g = dest_group(dest_order[--i]);    // already the first: the page before
            while (i > 0 && dest_group(dest_order[i - 1]) == g)
                i--;
        }
        return dest_order[i];
    }
    case C_MWCC:
    case C_ATCC: {
        int lo = kind == C_MWCC ? 1 : 0;
        v = d > 0 ? (v / 10 + 1) * 10 : (v % 10 ? v / 10 * 10 : v - 10);
        return within(v, lo, CC_MAX);
    }
    default:
        return v;
    }
}

// Move cell k of track t by delta (whole steps; for a depth, 1/256 steps);
// snap: FUNC + turn, the next useful value in delta's direction.
void me_page_change(int t, int k, int delta, int snapping)
{
    const struct cell *c = cell_of(t, k);
    int v = me_page_value(t, k);
    if (c->kind == C_NONE || !delta)
        return;
    if (snapping && c->kind != C_ONOFF) {
        set_value(t, k, snap(c->kind, v, delta));
        return;
    }
    switch (c->kind) {
    case C_RANGE:  v = within(v + delta, 0, PB_RANGE_MAX); break;
    case C_SENS:   v = within(v + delta, 0, VEL_SENS_MAX); break;
    case C_AMOUNT: v = within(v + delta, -AMOUNT_MAX, AMOUNT_MAX); break;
    case C_DEST:   v = dest_order[within(dest_index(v) + delta, 0, DEST_COUNT - 1)]; break;
    case C_ONOFF:  v = delta > 0; break;
    case C_MWCC:   v = within(v + delta, 1, CC_MAX); break;
    case C_ATCC:   v = within(v + delta, 0, CC_MAX); break;
    }
    set_value(t, k, v);
}

// ---- drawing --------------------------------------------------------------------
// y grows upwards: 0 is the bottom row of pixels, 63 the top. Measured off
// the stock LFO page: the cells' columns start at x = 24 + 26 * column, the
// top row's graphics at y 34..50 with the labels at 27, the bottom row's at
// 7..23 with the labels at 0; the header's white bar runs x 17..127, y 54..63.
#define CELL_X(k)   (24 + 26 * ((k) & 3))
#define CELL_Y(k)   ((k) < 4 ? 34 : 7)

static int text_width(const void *font, const char *s)
{
#ifdef ME_HOST_TEST
    (void)font;
    int n = 0;
    while (s[n])
        n++;
    return n ? 4 * n - 1 : 0;
#else
    if (font == FONT5) {                // fixed width: 3-pixel glyphs and a gap
        int n = 0;
        while (s[n])
            n++;
        return n ? 4 * n - 1 : 0;
    }
    const unsigned char *w = ((const unsigned char *const *)font)[2];
    int a = 0;
    for (; *s; s++)
        a += w[(unsigned char)*s] + 1;
    return a ? a - 1 : 0;
#endif
}

static void centred(void *bmp, const void *font, int cx, int y, const char *s)
{
    TEXT(bmp, font, cx - text_width(font, s) / 2, y, -1, "%s", s);
}

// A box like the LFO page's MULT and DEST cells: corners left open.
static void box(void *bmp, int x0, int y0, int x1, int y1)
{
    FILLRECT(bmp, x0 + 1, y1, x1 - 1, y1, 1);
    FILLRECT(bmp, x0 + 1, y0, x1 - 1, y0, 1);
    FILLRECT(bmp, x0, y0 + 1, x0, y1 - 1, 1);
    FILLRECT(bmp, x1, y0 + 1, x1, y1 - 1, 1);
}

// The stock TRIG page's empty cell: a dotted box.
static void dotted(void *bmp, int x, int y)
{
    for (int i = 0; i < 8; i++) {
        FILLRECT(bmp, x + 2 + 2 * i, y + 16, x + 2 + 2 * i, y + 16, 1);
        FILLRECT(bmp, x + 2 + 2 * i, y, x + 2 + 2 * i, y, 1);
        FILLRECT(bmp, x + 1, y + 1 + 2 * i, x + 1, y + 1 + 2 * i, 1);
        FILLRECT(bmp, x + 17, y + 1 + 2 * i, x + 17, y + 1 + 2 * i, 1);
    }
}

static void knob(void *bmp, int x, int y, int pos127, int bipolar)
{
    KNOB_GFX(0, 148, within(pos127, 0, 127) << 8, 0, 0, bmp, x, y);
    if (bipolar) {                      // the stock pages' - and + under a bipolar knob
        FILLRECT(bmp, x - 1, y + 1, x + 1, y + 1, 1);
        FILLRECT(bmp, x + 17, y + 1, x + 19, y + 1, 1);
        FILLRECT(bmp, x + 18, y, x + 18, y + 2, 1);
    }
}

static void format_value(char *b, int t, int k)
{
    int v = me_page_value(t, k);
    const char *s = 0;
    switch (cell_of(t, k)->kind) {
    case C_RANGE:  if (v <= 0) s = "OFF"; else { b[0] = (char)('0' + v / 10); b[1] = (char)('0' + v % 10); b[2] = 0; if (v < 10) { b[0] = b[1]; b[1] = 0; } } break;
    case C_AMOUNT: {                    // as the LFO page's DEP: +12.50
        int a = v < 0 ? -v : v, w = a >> 8, f = ((a & 255) * 100 + 128) >> 8, i = 0;
        if (f > 99)
            f = 99;
        b[i++] = v > 0 ? '+' : v < 0 ? '-' : ' ';
        if (w >= 10)
            b[i++] = (char)('0' + w / 10);
        b[i++] = (char)('0' + w % 10);
        b[i++] = '.';
        b[i++] = (char)('0' + f / 10);
        b[i++] = (char)('0' + f % 10);
        b[i] = 0;
        if (b[0] == ' ')
            for (int j = 0; j <= i; j++)
                b[j] = b[j + 1];
        break;
    }
    case C_SENS:
    case C_MWCC:
    case C_ATCC: {
        char d[6];
        int n = 0, a = v < 0 ? -v : v, i = 0;
        if (v < 0) b[i++] = '-';
        do { d[n++] = (char)('0' + a % 10); a /= 10; } while (a);
        while (n) b[i++] = d[--n];
        if (cell_of(t, k)->kind == C_SENS) b[i++] = '%';
        b[i] = 0;
        break;
    }
    case C_ONOFF:  s = v ? "ON" : "OFF"; break;
    default:       s = dest_short(v); break;
    }
    if (s) {
        int i = 0;
        for (; s[i] && i < 7; i++)
            b[i] = s[i];
        b[i] = 0;
    }
}

static void draw_cell(void *bmp, int t, int k)
{
    const struct cell *c = cell_of(t, k);
    int x = CELL_X(k), y = CELL_Y(k), v = me_page_value(t, k);
    char val[8];
    format_value(val, t, k);
    switch (c->kind) {
    case C_NONE:
        dotted(bmp, x, y);
        break;
    case C_RANGE:                       // a box with the semitones, as LFO MULT shows BPM 16
        box(bmp, x + 1, y, x + 17, y + 16);
        centred(bmp, FONT5, x + 9, y + 9, "ST");
        centred(bmp, FONT5, x + 9, y + 2, val);
        break;
    case C_SENS:
        knob(bmp, x, y, v * 127 / VEL_SENS_MAX, 0);
        break;
    case C_AMOUNT:
        knob(bmp, x, y, (v + AMOUNT_MAX) * 127 / (2 * AMOUNT_MAX), 1);
        break;
    case C_ONOFF:
        box(bmp, x + 1, y + 4, x + 17, y + 12);
        if (v) {                        // ON: the box filled, the word dark
            centred(bmp, FONT5, x + 9, y + 6, "ON");
            FILLRECT(bmp, x + 2, y + 5, x + 16, y + 11, -1);
        } else {
            centred(bmp, FONT5, x + 9, y + 6, "OFF");
        }
        break;
    case C_MWCC:
    case C_ATCC:
    case C_DEST: {                      // a wide box like the LFO page's DEST
        const char *top = c->kind == C_DEST ? dest_page(v) : c->kind == C_ATCC && !v ? "" : "CC";
        const char *bot = c->kind == C_DEST ? dest_short(v) : c->kind == C_ATCC && !v ? "AT" : val;
        box(bmp, x - 1, y, x + 19, y + 16);
        if (*top) {
            centred(bmp, LABEL_FONT, x + 9, y + 9, top);
            centred(bmp, LABEL_FONT, x + 9, y + 2, bot);
        } else {
            centred(bmp, LABEL_FONT, x + 9, y + 6, bot);
        }
        break;
    }
    }
    // the label under it, or the value for a moment after a turn
    centred(bmp, LABEL_FONT, x + 9, y - 7, shown[k] && c->kind != C_DEST && c->kind != C_NONE ? val : c->label);
}

// The LFO page's destination list: the right of the page, seven rows, the
// chosen one inverted, a scroll bar (measured off the stock list).
static void draw_list(void *bmp, int t, int k)
{
    int cur = dest_index(me_page_value(t, k));
    int first = within(cur - 3, 0, DEST_COUNT > 7 ? DEST_COUNT - 7 : 0);
    FILLRECT(bmp, 50, 0, 127, 53, 0);
    for (int r = 0; r < 7 && first + r < DEST_COUNT; r++) {
        int s = dest_order[first + r], y = 45 - 7 * r;
        char line[24];
        int n = 0;
        const char *p = s ? dest_page(s) : "None", *name = s ? STOCK_NAME(0, slot_id(s)) : "";
        if (!name)
            name = dest_short(s);
        while (*p && n < 23)
            line[n++] = *p++;
        if (s && n < 23)
            line[n++] = ':';
        while (*name && n < 23)
            line[n++] = *name++;
        line[n] = 0;
        while (n > 1 && text_width(LABEL_FONT, line) > 70)
            line[--n] = 0;              // what does not fit before the scroll bar
        TEXT(bmp, LABEL_FONT, 52, y, -1, "%s", line);
        if (first + r == cur)
            FILLRECT(bmp, 51, y - 1, 122, y + 5, -1);
    }
    for (int y = 0; y < 52; y += 2)     // the bar's track
        FILLRECT(bmp, 125 + (y >> 1 & 1), y, 125 + (y >> 1 & 1), y, 1);
    int len = 52 * 7 / DEST_COUNT, top = 52 - (52 - len) * first / (DEST_COUNT - 7);
    FILLRECT(bmp, 124, top - len, 127, top, 0);
    box(bmp, 124, top - len, 127, top);
}

// Over the header's title ("Trig Parameters"): ours, in the same font.
static void draw_title(void *bmp)
{
    FILLRECT(bmp, 18, 54, 126, 63, 0);
    FILLRECT(bmp, 17, 55, 127, 62, 0);
    TEXT(bmp, TITLE_FONT, 19, 56, -1, "%s", "Expression");
    FILLRECT(bmp, 18, 54, 126, 63, -1);
    FILLRECT(bmp, 17, 55, 17, 62, -1);
    FILLRECT(bmp, 127, 55, 127, 62, -1);
}

void me_page_draw(void *bmp, void *ctrl)
{
    if (ctrl)
        ctrl_seen = ctrl;
    if (!me_page_open || !bmp)
        return;
    if (!trig_page(ctrl)) {             // another page or a menu took over
        close_page();
        return;
    }
    int t = track();
    FILLRECT(bmp, 16, 0, 127, 53, 0);   // the stock track box and LEV meter stay
    draw_title(bmp);
    for (int k = 0; k < CELLS; k++)
        draw_cell(bmp, t, k);
    if (list_cell >= 0 && cell_of(t, list_cell)->kind == C_DEST)
        draw_list(bmp, t, list_cell);
}

// ---- ev_tick (from me_tick) -----------------------------------------------------
void me_page_tick(void *ctrl)
{
    if (ctrl)
        ctrl_seen = ctrl;
    if (pend && hold < 255)
        hold++;
    if (++ticks >= 3) {                 // the firmware decays its accelerators ten times a second
        ticks = 0;
        enc_decay();
    }
    for (int k = 0; k < CELLS; k++) {
        if (shown[k])
            shown[k]--;
        if (snap_rest[k])
            snap_rest[k]--;
    }
    if (list_t && !--list_t)
        list_cell = -1;
    if ((me_page_open || recompose) && ctrl) {
        *((volatile char *)ctrl + 0x20) = 1;           // keep the frames coming
        recompose = 0;
    }
}

// ---- ev_key -----------------------------------------------------------------------
static void open_page(void)
{
    me_page_open = 1;
    list_cell = -1;
    enc_reset();
    for (int k = 0; k < CELLS; k++)
        acc[k] = shown[k] = 0;
}

int me_page_key(void *brain, void *ev)
{
    int id = KEYID(ev);
    void *ctrl = brain ? (char *)brain + 0x40 : ctrl_seen;
    if (!me_page_open) {
        if (id == K_TRIG) {
            if (KEYPRESS(ev)) {
                pend = !KEYHELD(K_FUNC) && trig_page(ctrl) != 0;
                hold = 0;
            } else if (!KEYDOWN(ev) && pend) {
                pend = 0;
                if (hold < TAP_TICKS && trig_page(ctrl))
                    open_page();
            }
        } else if (pend && KEYPRESS(ev)) {
            pend = 0;                   // TRIG + another key: the stock page's gesture
        }
        return 0;                       // the stock page always sees these keys
    }
    if (KEYPRESS(ev) && (!(id >= K_PUSH_A && id < K_PUSH_A + CELLS) || id - K_PUSH_A == list_cell))
        list_cell = -1;                 // any key, or a push of its own knob, closes the list
    switch (id) {
    case K_TRIG:
        if (KEYHELD(K_FUNC)) {          // FUNC + TRIG: the stock quantize page
            close_page();
            return 0;
        }
        if (!KEYDOWN(ev))
            close_page();               // a tap of TRIG goes back to the first page
        return 1;
    case K_NO:
        if (KEYPRESS(ev))
            close_page();
        return 1;
    case K_YES: case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT:
        return 1;                       // nothing here; not passed to the page underneath
    case 1:                             // FUNC
    case 2:                             // TRK: TRK + trig picks another track
    case 3:                             // PTN
    case 4:                             // BANK
    case 9: case 10: case 11:           // REC, PLAY, STOP
        return 0;
    default:
        if (id >= 24 && id <= 39)       // the trig keys
            return 0;
        if (id >= K_PUSH_A && id < K_PUSH_A + CELLS)
            return 1;                   // the knobs' pushes: the push multiplier (and the list above)
        if (KEYPRESS(ev))
            close_page();               // another page or a menu: stock takes it from here
        return 0;
    }
}

// ---- ev_enc -----------------------------------------------------------------------
int me_page_enc(void *brain, void *ev)
{
    (void)brain;
    if (!me_page_open)
        return 0;
    int id = *(int *)((char *)ev + 12);
    int n = *(int *)((char *)ev + 16);
    if (id < 1 || id > CELLS)
        return 0;                       // LEVEL: the track level, as stock
    if (!n)
        return 1;
    int k = id - 1, t = track();
    const struct cell *c = cell_of(t, k);
    if (t != acc_track) {
        for (int i = 0; i < CELLS; i++)
            acc[i] = 0;
        acc_track = t;
    }
    if (list_cell != k)
        list_cell = -1;
    if (c->kind == C_NONE)
        return 1;
    if (KEYHELD(K_FUNC)) {              // FUNC + turn: the next useful value, then a rest
        if (!snap_rest[k]) {
            me_page_change(t, k, n, 1);
            snap_rest[k] = SNAP_REST;
        }
        acc[k] = 0;
    } else {
        int w = enc_words(k, sid_of(c->kind), n, KEYHELD(K_PUSH_A + k) != 0);
        if (w) {
            idle_counts[k] = 0;
        } else {                        // a safety net: six detents and still nothing,
            idle_counts[k] += n < 0 ? -n : n;   // as the firmware never does: a step a detent
            if (idle_counts[k] >= IDLE_LIMIT)
                w = n * 256 / RAW_DETENT;
        }
        if (c->kind == C_AMOUNT) {      // a depth takes the words as they are: 1/256 steps
            me_page_change(t, k, w, 0);
        } else {
            acc[k] += w;
            int steps = acc[k] / 256;
            acc[k] -= steps * 256;
            if (c->kind == C_ONOFF && !steps && n)
                steps = n > 0 ? 1 : -1; // a switch follows the first detent
            me_page_change(t, k, steps, 0);
        }
    }
    shown[k] = SHOW_TICKS;
    if (c->kind == C_DEST) {
        list_cell = k;
        list_t = LIST_TICKS;
    }
    return 1;                           // the cells' knobs are the page's while it is up
}
