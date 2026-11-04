// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI Expression: the TRIG page's second page. Each track's own settings:
//
//   audio track   A BEND RANGE  OFF, 1..12 semitones (default 2)
//                 B MW DEST     OFF or a sound parameter (default OFF)
//                 C MW AMOUNT   -64..+64 (default +32)
//                 D AT DEST     OFF or a sound parameter (default OFF)
//                 E AT AMOUNT   -64..+64 (default +32)
//   MIDI track    A PITCH BEND  ON/OFF (default ON)
//                 B AFTERTOUCH  ON/OFF (default ON)
//                 C MOD WHEEL   ON/OFF (default ON)
//                 D MW AS       CC 1 (the track's own MW, default) or CC 2..119
//                 E AT AS       AFTERTOUCH (default) or CC 1..119
//
// Opening it. On the TRIG page, a tap of TRIG (stock: nothing; a hold stays
// the stock page's, showing its values, with TRIG + REC/STOP/PLAY). FUNC +
// TRIG stays the stock quantize page. Leaving: TRIG or NO, or any other page
// or menu key, which then does its stock job. The trig keys, TRK + trig,
// FUNC, PTN, BANK and the transport keep working under the page.
//
// Editing. Knobs A-E turn their row's value; UP/DOWN or the LEVEL knob move
// the cursor, LEFT/RIGHT change its value, YES toggles an ON/OFF row.
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

struct me_audio_cfg me_audio_cfg[8] = {
    { 2, 0, 0, 32, 32 }, { 2, 0, 0, 32, 32 }, { 2, 0, 0, 32, 32 }, { 2, 0, 0, 32, 32 },
    { 2, 0, 0, 32, 32 }, { 2, 0, 0, 32, 32 }, { 2, 0, 0, 32, 32 }, { 2, 0, 0, 32, 32 },
};

struct me_midi_cfg me_midi_cfg[8] = {
    { 1, 1, 1, 1, 0 }, { 1, 1, 1, 1, 0 }, { 1, 1, 1, 1, 0 }, { 1, 1, 1, 1, 0 },
    { 1, 1, 1, 1, 0 }, { 1, 1, 1, 1, 0 }, { 1, 1, 1, 1, 0 }, { 1, 1, 1, 1, 0 },
};

#ifdef ME_HOST_TEST
#include "host_page.h"
#else
#define ACTIVE_TRACK_P (*(volatile int32_t *)0x4197c6b4)
#define TEXT       ((void (*)(void *, const void *, int, int, int, const char *, ...))0x400c27a4)
#define FILLRECT   ((void (*)(void *, int, int, int, int, int))0x400c1bce)
#define FONT5      ((const void *)0x40200ebc)
#define KEYHELD(k) (((uint32_t (*)(int))0x400d399c)(k))
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
#endif

#define KEYID(ev)    (*(int *)((char *)(ev) + 12))
#define KEYDOWN(ev)  (*(int *)((char *)(ev) + 16) & 1)
#define KEYPRESS(ev) ((*(int *)((char *)(ev) + 16) & 9) == 1)   // down, not a repeat

enum {
    K_FUNC = 1, K_YES = 12, K_NO = 13, K_UP = 14, K_DOWN = 15, K_LEFT = 16, K_RIGHT = 17,
    K_TRIG = 19,
};
#define TAP_TICKS 15                    // half a second at 30 Hz: shorter is a tap
#define ROWS      5

int me_page_open;                       // read by the tests
static int cursor;
static int pend;                        // TRIG went down on the TRIG page
static int hold;                        // UI ticks since
static void *ctrl_seen;                 // the view controller, from ev_tick / ev_draw
static int recompose;                   // the page just closed: one more frame for the stock page

static void close_page(void)
{
    if (me_page_open)
        recompose = 1;
    me_page_open = 0;
}

// ---- the values ---------------------------------------------------------------
// The sound's parameter slots a route can move (Digi Matrix's measured map).
static const char *const slot_names[46] = {
    "OFF",
    "L1.SPD", "L1.MUL", "L1.FADE", "L1.DEST", "L1.WAVE", "L1.PHAS", "L1.MODE", "L1.DEP",
    "L2.SPD", "L2.MUL", "L2.FADE", "L2.DEST", "L2.WAVE", "L2.PHAS", "L2.MODE", "L2.DEP",
    "SRC.A", "SRC.B", "SRC.C", "SRC.D", "SRC.E", "SRC.F", "SRC.G", "SRC.H",
    "FLT.TYPE", "FLT.FREQ", "FLT.RESO", "FLT.ENV", "FLT.ATK", "FLT.DEC", "FLT.SUS", "FLT.REL",
    "FLT2.A", "FLT2.B", "FLT2.C", "FLT2.D", "FLT2.E",
    "AMP.ATK", "AMP.HOLD", "AMP.DEC", "AMP.OVER", "AMP.DEL", "AMP.REV", "AMP.PAN", "AMP.VOL",
};

// The destinations, in the order they step: the sound's pages, then the LFOs.
// The LFO DEST, WAVE and MODE slots and FLT.TYPE are choices, not amounts.
static const int8_t dest_order[] = {
    0,
    17, 18, 19, 20, 21, 22, 23, 24,             /* SRC.A-H */
    26, 27, 28, 29, 30, 31, 32,                 /* FLT.FREQ ... FLT.REL */
    33, 34, 35, 36, 37,                         /* FLT2.A-E */
    38, 39, 40, 41, 42, 43, 44, 45,             /* AMP */
    1, 2, 3, 6, 8,                              /* L1: SPD, MUL, FADE, PHAS, DEP */
    9, 10, 11, 14, 16,                          /* L2 */
};
#define DEST_COUNT ((int)(sizeof dest_order / sizeof dest_order[0]))

static int step_dest(int slot, int n)
{
    int i = 0;
    while (i < DEST_COUNT && dest_order[i] != slot)
        i++;
    if (i == DEST_COUNT)
        i = 0;
    i += n;
    i = i < 0 ? 0 : i >= DEST_COUNT ? DEST_COUNT - 1 : i;
    return dest_order[i];
}

static int within(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static int sign(int d)
{
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

static int track(void)
{
    int t = ACTIVE_TRACK_P;
    return t >= 0 && t < 16 ? t : 0;
}

// Change row `row` of the shown track by n steps (a knob's count, or +-1).
// toggle: YES on an ON/OFF row.
void me_page_change(int row, int n, int toggle)
{
    int t = track();
    if (t < 8) {
        struct me_audio_cfg *a = &me_audio_cfg[t];
        if (toggle)
            return;
        switch (row) {
        case 0: a->range = (int8_t)within(a->range + sign(n), 0, PB_RANGE_MAX); break;
        case 1: a->mw_dest = (int8_t)step_dest(a->mw_dest, sign(n)); break;
        case 2: a->mw_amount = (int8_t)within(a->mw_amount + n, -AMOUNT_MAX, AMOUNT_MAX); break;
        case 3: a->at_dest = (int8_t)step_dest(a->at_dest, sign(n)); break;
        case 4: a->at_amount = (int8_t)within(a->at_amount + n, -AMOUNT_MAX, AMOUNT_MAX); break;
        }
        return;
    }
    struct me_midi_cfg *m = &me_midi_cfg[t - 8];
    volatile uint8_t *flag = row == 0 ? &m->pb_on : row == 1 ? &m->at_on : row == 2 ? &m->mw_on : 0;
    uint8_t before[5] = { m->pb_on, m->at_on, m->mw_on, m->mw_cc, m->at_cc };
    if (flag) {
        if (toggle)
            *flag = !*flag;
        else if (n)
            *flag = n > 0;
    } else if (!toggle && row == 3) {
        m->mw_cc = (uint8_t)within(m->mw_cc + n, 1, CC_MAX);
    } else if (!toggle && row == 4) {
        m->at_cc = (uint8_t)within(m->at_cc + n, 0, CC_MAX);
    }
    if (before[0] != m->pb_on || before[1] != m->at_on || before[2] != m->mw_on ||
        before[3] != m->mw_cc || before[4] != m->at_cc)
        me_midi_release_track(t - 8);
}

// ---- drawing --------------------------------------------------------------------
// y grows upwards: 0 is the bottom row of pixels, 63 the top.
#define ROWY(i)  (45 - (i) * 8)
#define LABEL_X  9
#define VALUE_X  70

static const char *const audio_labels[ROWS] = { "BEND RANGE", "MW DEST", "MW AMOUNT", "AT DEST", "AT AMOUNT" };
static const char *const midi_labels[ROWS] = { "PITCH BEND", "AFTERTOUCH", "MOD WHEEL", "MW AS", "AT AS" };

static void draw_amount(void *bmp, int y, int v)
{
    if (v > 0)
        TEXT(bmp, FONT5, VALUE_X, y, -1, "+%d", v);
    else if (v < 0)
        TEXT(bmp, FONT5, VALUE_X, y, -1, "-%d", -v);
    else
        TEXT(bmp, FONT5, VALUE_X, y, -1, "0");
}

static const char *dest_name(int s)
{
    return slot_names[s >= 0 && s <= 45 ? s : 0];
}

static void draw_value(void *bmp, int t, int row, int y)
{
    if (t < 8) {
        const struct me_audio_cfg *a = &me_audio_cfg[t];
        switch (row) {
        case 0:
            if (a->range <= 0)
                TEXT(bmp, FONT5, VALUE_X, y, -1, "OFF");
            else
                TEXT(bmp, FONT5, VALUE_X, y, -1, "%d ST", a->range);
            break;
        case 1: TEXT(bmp, FONT5, VALUE_X, y, -1, "%s", dest_name(a->mw_dest)); break;
        case 2: draw_amount(bmp, y, a->mw_amount); break;
        case 3: TEXT(bmp, FONT5, VALUE_X, y, -1, "%s", dest_name(a->at_dest)); break;
        case 4: draw_amount(bmp, y, a->at_amount); break;
        }
        return;
    }
    const struct me_midi_cfg *m = &me_midi_cfg[t - 8];
    switch (row) {
    case 0: TEXT(bmp, FONT5, VALUE_X, y, -1, "%s", m->pb_on ? "ON" : "OFF"); break;
    case 1: TEXT(bmp, FONT5, VALUE_X, y, -1, "%s", m->at_on ? "ON" : "OFF"); break;
    case 2: TEXT(bmp, FONT5, VALUE_X, y, -1, "%s", m->mw_on ? "ON" : "OFF"); break;
    case 3:
        if (m->mw_cc == 1)
            TEXT(bmp, FONT5, VALUE_X, y, -1, "CC 1 (MW)");
        else
            TEXT(bmp, FONT5, VALUE_X, y, -1, "CC %d", m->mw_cc);
        break;
    case 4:
        if (!m->at_cc)
            TEXT(bmp, FONT5, VALUE_X, y, -1, "AFTERTCH");
        else
            TEXT(bmp, FONT5, VALUE_X, y, -1, "CC %d", m->at_cc);
        break;
    }
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
    FILLRECT(bmp, 0, 0, 127, 63, 0);
    if (t < 8)
        TEXT(bmp, FONT5, 1, 56, -1, "EXPRESSION: TRACK %d", t + 1);
    else
        TEXT(bmp, FONT5, 1, 56, -1, "EXPRESSION: MIDI %c", 'A' + (t - 8));
    FILLRECT(bmp, 0, 53, 127, 53, 1);
    if (!me_pb_on && !me_at_on && !me_mw_on)
        TEXT(bmp, FONT5, 1, 2, -1, "THRU OFF (SETTINGS)");
    for (int r = 0; r < ROWS; r++) {
        int y = ROWY(r);
        TEXT(bmp, FONT5, 1, y, -1, "%c", 'A' + r);
        TEXT(bmp, FONT5, LABEL_X, y, -1, "%s", t < 8 ? audio_labels[r] : midi_labels[r]);
        draw_value(bmp, t, r, y);
        if (r == cursor)
            FILLRECT(bmp, 0, y - 1, 127, y + 5, -1);   // the cursor's row, inverted
    }
}

// ---- ev_tick (from me_tick) -----------------------------------------------------
void me_page_tick(void *ctrl)
{
    if (ctrl)
        ctrl_seen = ctrl;
    if (pend && hold < 255)
        hold++;
    if ((me_page_open || recompose) && ctrl) {
        *((volatile char *)ctrl + 0x20) = 1;           // keep the frames coming
        recompose = 0;
    }
}

// ---- ev_key -----------------------------------------------------------------------
static void open_page(void)
{
    me_page_open = 1;
    cursor = 0;
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
    switch (id) {
    case K_TRIG:
        if (KEYHELD(K_FUNC)) {          // FUNC + TRIG: the stock quantize page
            close_page();
            return 0;
        }
        if (!KEYDOWN(ev))
            close_page();           // a tap of TRIG goes back to the first page
        return 1;
    case K_NO:
        if (KEYPRESS(ev))
            close_page();
        return 1;
    case K_UP:
        if (KEYDOWN(ev) && cursor > 0)
            cursor--;
        return 1;
    case K_DOWN:
        if (KEYDOWN(ev) && cursor < ROWS - 1)
            cursor++;
        return 1;
    case K_LEFT:
    case K_RIGHT:
        if (KEYDOWN(ev))
            me_page_change(cursor, id == K_RIGHT ? 1 : -1, 0);
        return 1;
    case K_YES:
        if (KEYPRESS(ev))
            me_page_change(cursor, 0, 1);
        return 1;
    case 1:                             // FUNC
    case 2:                             // TRK: TRK + trig picks another track
    case 3:                             // PTN
    case 4:                             // BANK
    case 9: case 10: case 11:           // REC, PLAY, STOP
        return 0;
    default:
        if (id >= 24 && id <= 39)       // the trig keys
            return 0;
        if (id >= 40 && id <= 47)       // the knobs' pushes: nothing here
            return 1;
        if (KEYPRESS(ev))
            close_page();           // another page or a menu: stock takes it from here
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
    if (!n)
        return 1;
    if (id >= 1 && id <= ROWS) {        // knobs A-E: their row
        cursor = id - 1;
        me_page_change(cursor, n, 0);
    } else if (id == 9) {               // LEVEL: the cursor
        cursor = within(cursor + sign(n), 0, ROWS - 1);
    }
    return 1;                           // every knob is the page's while it is up
}
