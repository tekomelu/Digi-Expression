// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI Expression: the value rows in SETTINGS (BEND RANGE, MW DEST, MW AMT,
// AT DEST, AT AMT). LEFT/RIGHT change the value; YES does nothing.
// The row record's label is built in rows.s; select, draw and change are
// here. Drawing follows Digi Matrix's row (gdeo607): the firmware's text
// call with its 5-pixel font, at OS 1.54's addresses.

#include "expr.h"

#ifdef ME_HOST_TEST
void h_invalidate(void *view);
void h_text(void *bmp, const void *font, int x, int y, int colour, const char *fmt, ...);
#define INVALIDATE h_invalidate
#define TEXT       h_text
#define FONT5      ((const void *)0)
#else
#define INVALIDATE ((void (*)(void *))0x400c9a3a)
#define TEXT       ((void (*)(void *, const void *, int, int, int, const char *, ...))0x400c27a4)
#define FONT5      ((const void *)0x40200ebc)
#endif

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

// The destinations, in the order LEFT/RIGHT steps through them: the sound's
// pages first, then the LFOs. The LFO DEST, WAVE and MODE slots and FLT.TYPE
// are choices, not amounts, so a wheel would jump through them: left out.
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

static int step_dest(int slot, int delta)
{
    int i = 0;
    while (i < DEST_COUNT && dest_order[i] != slot)
        i++;
    if (i == DEST_COUNT)
        i = 0;
    i += delta > 0 ? 1 : delta < 0 ? -1 : 0;
    if (i < 0)
        i = 0;
    if (i >= DEST_COUNT)
        i = DEST_COUNT - 1;
    return dest_order[i];
}

#define VALUE_X 48                  /* where the value starts, from the row's x */

static void redraw(void **payload)
{
    INVALIDATE((char *)*payload + 0x38);
}

static void draw_amount(void *bmp, int x, int y, int v)
{
    if (v > 0)
        TEXT(bmp, FONT5, x + VALUE_X, y, -1, "+%d", v);
    else if (v < 0)
        TEXT(bmp, FONT5, x + VALUE_X, y, -1, "-%d", -v);
    else
        TEXT(bmp, FONT5, x + VALUE_X, y, -1, "0");
}

static int8_t clamp_amount(int v)
{
    return (int8_t)(v < -64 ? -64 : v > 64 ? 64 : v);
}

void me_row_none(void **payload) { (void)payload; }


// BEND RANGE: OFF, 1..24 semitones.
void me_range_draw(void *a, void *b, void *bmp, int x, int y)
{
    (void)a; (void)b;
    if (me_bend_range <= 0)
        TEXT(bmp, FONT5, x + VALUE_X, y, -1, "OFF");
    else
        TEXT(bmp, FONT5, x + VALUE_X, y, -1, "%d ST", me_bend_range);
}
void me_range_change(void **payload, int unused, int delta)
{
    (void)unused;
    int v = me_bend_range + (delta > 0 ? 1 : delta < 0 ? -1 : 0);
    me_bend_range = (int8_t)(v < 0 ? 0 : v > 24 ? 24 : v);
    redraw(payload);
}

// MW DEST / AT DEST
void me_mwdest_draw(void *a, void *b, void *bmp, int x, int y)
{
    (void)a; (void)b;
    TEXT(bmp, FONT5, x + VALUE_X, y, -1, "%s", slot_names[me_mw_dest >= 0 && me_mw_dest <= 45 ? me_mw_dest : 0]);
}
void me_mwdest_change(void **payload, int unused, int delta)
{
    (void)unused;
    if (delta)
        me_mw_dest = (int8_t)step_dest(me_mw_dest, delta);
    redraw(payload);
}
void me_atdest_draw(void *a, void *b, void *bmp, int x, int y)
{
    (void)a; (void)b;
    TEXT(bmp, FONT5, x + VALUE_X, y, -1, "%s", slot_names[me_at_dest >= 0 && me_at_dest <= 45 ? me_at_dest : 0]);
}
void me_atdest_change(void **payload, int unused, int delta)
{
    (void)unused;
    if (delta)
        me_at_dest = (int8_t)step_dest(me_at_dest, delta);
    redraw(payload);
}

// MW AMT / AT AMT: -64..+64
void me_mwamt_draw(void *a, void *b, void *bmp, int x, int y)
{
    (void)a; (void)b;
    draw_amount(bmp, x, y, me_mw_amount);
}
void me_mwamt_change(void **payload, int unused, int delta)
{
    (void)unused;
    me_mw_amount = clamp_amount(me_mw_amount + (delta > 0 ? 1 : delta < 0 ? -1 : 0));
    redraw(payload);
}
void me_atamt_draw(void *a, void *b, void *bmp, int x, int y)
{
    (void)a; (void)b;
    draw_amount(bmp, x, y, me_at_amount);
}
void me_atamt_change(void **payload, int unused, int delta)
{
    (void)unused;
    me_at_amount = clamp_amount(me_at_amount + (delta > 0 ? 1 : delta < 0 ? -1 : 0));
    redraw(payload);
}
