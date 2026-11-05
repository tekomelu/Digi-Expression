// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI Expression: each track's EXPRESSION settings, kept with the project.
//
// No byte of a saved sound or pattern is free for them (docs/RESEARCH-OS154.md,
// section 13), so they live in a file of their own on the +Drive, as Digi
// Slicer keeps its slices: /cfw/expression.a and /cfw/expression.b, written in
// turn (a power cut during a save leaves the other one whole; the loader takes
// the valid one with the higher sequence number), each rewritten in place
// with one write through the firmware's own ekFS calls.
//
// The file holds one record per project name, and the working record:
//   - SAVE PROJECT AS (and every project save): the project's record becomes
//     the settings as they are. The firmware formats "SAVING '%.16s'" with the
//     project's name; a 6-byte jsr site over that sprintf call (store.s)
//     hands the name here first.
//   - LOAD PROJECT: the same for "LOADING '%.16s'": the project's record is
//     loaded (a project saved without one gets the defaults).
//   - Any change on the page: the working record, a second later. At power-on
//     the working record comes back: the settings as they were when the unit
//     was switched off, as the Digitakt brings back its working project.
// Two projects with the same name share a record.
//
// All of it runs in the UI task (ev_tick, 30 Hz). The ekFS calls take the
// filesystem's mutex themselves; this waits while it is held (a project or
// sample load holds it) and while the drive is not mounted.

#include "expr.h"

typedef uint32_t u32;
typedef int32_t s32;

#ifdef ME_HOST_TEST
#include "host_store.h"
#else
typedef struct { u32 inode, pos, writable, open; } ekfile;
#define EK_OPEN    ((s32 (*)(const char *, const char *, ekfile *))0x400cf3a0)
#define EK_READ    ((s32 (*)(void *, s32, ekfile *))0x400cf0e6)
#define EK_WRITE   ((s32 (*)(const void *, s32, ekfile *))0x400cf146)
#define EK_CLOSE   ((s32 (*)(ekfile *))0x400cf35e)
#define EK_MKDIR   ((s32 (*)(const char *))0x400ce034)
#define EK_LOOKUP  ((s32 (*)(const char *, u32 *, s32, s32, s32))0x400d0ccc)
#define EK_LOCK    ((void (*)(void *))0x40001884)
#define EK_UNLOCK  ((void (*)(void *))0x400019b6)
#define EK_MUTEX   ((void *)0x42686690)
#define EK_OWNER   (*(volatile u32 *)0x42686690)
#define EK_MOUNTED (*(volatile u32 *)0x420eec50)
#define NOT_FOUND  0x19
#endif

#define NREC      32                          // projects; the least recently saved goes
#define NAME_LEN  16
#define ST_MAGIC  0x4d455831u                // "MEX1"
#define SAVE_WAIT 30                          // ticks after the last change (1 s)
#define RETRY     300                         // after a failed save (10 s)

struct st_rec {
    char name[NAME_LEN];                      // the project's, as the firmware shows it
    u32 used;
    struct me_cfg audio[8], midi[8];
};

struct st_file {
    u32 magic, nrec, seq, sum;
    struct st_rec work;                       // as things are now (name: the project's)
    struct st_rec rec[NREC];                  // as each project was saved
    uint8_t pad[16];                          // a USB listing shows size - 16
};

struct st_file me_store;
static const char *const st_path[2] = { "/cfw/expression.b", "/cfw/expression.a" };
// For tests (found by its marker): [1] the file was read (or there was none),
// [2] saves, [3] failed saves, [4] reads. A test clears [1] to play a power-on.
volatile u32 me_store_probe[5] = { 0x4d455850, 0, 0, 0, 0 };   // "MEXP"
#define st_loaded      me_store_probe[1]
#define me_store_saves me_store_probe[2]
#define me_store_fails me_store_probe[3]
#define me_store_loads me_store_probe[4]
static s32 st_dirty, st_wait;
static char st_saving[NAME_LEN + 1], st_loading[NAME_LEN + 1];
static volatile uint8_t st_save_req, st_load_req;
static u32 st_last;                           // the settings' checksum last tick


static u32 sum_words(const void *p, u32 bytes, u32 s)
{
    const uint8_t *b = (const uint8_t *)p;
    for (u32 i = 0; i < bytes; i++)
        s = ((s << 5) | (s >> 27)) ^ b[i];
    return s;
}

static u32 file_sum(void)
{
    return sum_words(&me_store.work, sizeof me_store.work + sizeof me_store.rec, 0x5eed4d45u);
}

static u32 cfg_sum(void)
{
    return sum_words((const void *)me_midi_cfg, sizeof me_midi_cfg,
                     sum_words((const void *)me_audio_cfg, sizeof me_audio_cfg, 1));
}

static void copy_name(char *dst, const char *src)
{
    int i = 0;
    for (; src && src[i] && i < NAME_LEN; i++)
        dst[i] = src[i];
    for (; i < NAME_LEN; i++)
        dst[i] = 0;
}

static int same_name(const char *a, const char *b)
{
    for (int i = 0; i < NAME_LEN; i++) {
        if (a[i] != b[i])
            return 0;
        if (!a[i])
            return 1;
    }
    return 1;
}

static void copy_cfg(struct me_cfg *dst, const struct me_cfg *src)
{
    for (int t = 0; t < 8; t++) {
        dst[t].range = src[t].range;
        dst[t].mw_dest = src[t].mw_dest;
        dst[t].at_dest = src[t].at_dest;
        dst[t].vel_dest = src[t].vel_dest;
        dst[t].vel_sens = src[t].vel_sens;
        dst[t].mw_amount = src[t].mw_amount;
        dst[t].at_amount = src[t].at_amount;
        dst[t].vel_amount = src[t].vel_amount;
    }
}

// A record read from the card could hold anything: only values the page can set.
static void apply(const struct st_rec *r)
{
    for (int t = 0; t < 8; t++) {
        const struct me_cfg *a = &r->audio[t], *m = &r->midi[t];
        struct me_cfg *da = &me_audio_cfg[t], *dm = &me_midi_cfg[t];
#define IN(v, lo, hi, d) ((v) >= (lo) && (v) <= (hi) ? (v) : (d))
        da->range = (int8_t)IN(a->range, 0, PB_RANGE_MAX, 2);
        da->mw_dest = (int8_t)IN(a->mw_dest, 0, 45, 0);
        da->at_dest = (int8_t)IN(a->at_dest, 0, 45, 0);
        da->vel_dest = (int8_t)IN(a->vel_dest, 0, 45, 0);
        da->vel_sens = (int8_t)IN(a->vel_sens, 0, VEL_SENS_MAX, VEL_SENS_MAX);
        da->mw_amount = (int16_t)IN(a->mw_amount, -AMOUNT_MAX, AMOUNT_MAX, 0);
        da->at_amount = (int16_t)IN(a->at_amount, -AMOUNT_MAX, AMOUNT_MAX, 0);
        da->vel_amount = (int16_t)IN(a->vel_amount, -AMOUNT_MAX, AMOUNT_MAX, 0);
        dm->thru = (int8_t)IN(m->thru, 0, 1, 1);
        dm->mw_dest = (int8_t)IN(m->mw_dest, 0, MIDI_DESTS - 1, 0);
        dm->at_dest = (int8_t)IN(m->at_dest, 0, MIDI_DESTS - 1, 0);
        dm->vel_dest = (int8_t)IN(m->vel_dest, 0, MIDI_DESTS - 1, 0);
        dm->vel_sens = (int8_t)IN(m->vel_sens, 0, VEL_SENS_MAX, VEL_SENS_MAX);
        dm->mw_amount = (int16_t)IN(m->mw_amount, -AMOUNT_MAX, AMOUNT_MAX, 0);
        dm->at_amount = (int16_t)IN(m->at_amount, -AMOUNT_MAX, AMOUNT_MAX, 0);
        dm->vel_amount = (int16_t)IN(m->vel_amount, -AMOUNT_MAX, AMOUNT_MAX, 0);
#undef IN
    }
    for (int m = 0; m < 8; m++)
        me_midi_changed(m);
}

static void defaults(void)
{
    for (int t = 0; t < 8; t++) {
        me_audio_cfg[t].range = 2;
        me_midi_cfg[t].thru = 1;
        me_audio_cfg[t].vel_sens = me_midi_cfg[t].vel_sens = VEL_SENS_MAX;
        me_audio_cfg[t].mw_dest = me_audio_cfg[t].at_dest = me_audio_cfg[t].vel_dest = 0;
        me_midi_cfg[t].mw_dest = me_midi_cfg[t].at_dest = me_midi_cfg[t].vel_dest = 0;
        me_audio_cfg[t].mw_amount = me_audio_cfg[t].at_amount = me_audio_cfg[t].vel_amount = 0;
        me_midi_cfg[t].mw_amount = me_midi_cfg[t].at_amount = me_midi_cfg[t].vel_amount = 0;
    }
    for (int m = 0; m < 8; m++)
        me_midi_changed(m);
}

static void snapshot(struct st_rec *r, const char *name)
{
    copy_name(r->name, name);
    copy_cfg(r->audio, me_audio_cfg);
    copy_cfg(r->midi, me_midi_cfg);
    r->used = 1;
}

static struct st_rec *find(const char *name)
{
    for (int i = 0; i < NREC; i++)
        if (me_store.rec[i].used && same_name(me_store.rec[i].name, name))
            return &me_store.rec[i];
    return 0;
}

static void rec_move(struct st_rec *dst, const struct st_rec *src)
{
    volatile u32 *d = (volatile u32 *)dst;   // word by word: no memcpy in a mod
    const volatile u32 *sw = (const volatile u32 *)src;
    for (u32 k = 0; k < sizeof(struct st_rec) / 4; k++)
        d[k] = sw[k];
}

// The record a save goes to: the used ones stay in the order they were last
// saved, so the project's own record moves to the end, and a full store
// forgets the project saved longest ago.
static struct st_rec *place(const char *name)
{
    int i, n = 0;
    struct st_rec *r = find(name);
    int from = r ? (int)(r - me_store.rec) : 0;
    if (!r) {
        for (i = 0; i < NREC; i++)
            n += me_store.rec[i].used != 0;
        if (n < NREC) {                       // pack the used ones to the front first
            int w = 0;
            for (i = 0; i < NREC; i++)
                if (me_store.rec[i].used) {
                    if (w != i)
                        rec_move(&me_store.rec[w], &me_store.rec[i]);
                    w++;
                }
            for (i = w; i < NREC; i++)
                me_store.rec[i].used = 0;
            return &me_store.rec[w];
        }
        from = 0;                             // full: the oldest goes
    }
    for (i = from; i + 1 < NREC && me_store.rec[i + 1].used; i++)
        rec_move(&me_store.rec[i], &me_store.rec[i + 1]);
    return &me_store.rec[i];
}

// ---- the card ---------------------------------------------------------------
static s32 ek_ready(void)
{
    return EK_MOUNTED != 0 && EK_OWNER == 0;
}

static s32 img_read(const char *path)
{
    ekfile f;
    s32 got;
    if (EK_OPEN(path, "r", &f) != 0)
        return 0;
    got = EK_READ(&me_store, sizeof me_store, &f);
    EK_CLOSE(&f);
    return got >= (s32)(sizeof me_store - sizeof me_store.pad) && me_store.magic == ST_MAGIC
           && me_store.nrec == NREC && me_store.sum == file_sum();
}

static void clear_store(void)
{
    me_store.work.used = 0;
    for (int i = 0; i < NREC; i++)
        me_store.rec[i].used = 0;
    me_store.seq = 0;
}

// 0: the drive is busy, try again; 1: read (or there was nothing to read).
static s32 store_load(void)
{
    s32 ok0, ok1, pick;
    u32 seq0, seq1;
    if (!ek_ready())
        return 0;
    ok0 = img_read(st_path[0]);
    seq0 = me_store.seq;
    ok1 = img_read(st_path[1]);
    seq1 = me_store.seq;
    if (ok0 && ok1)
        pick = seq1 > seq0 ? 1 : 0;
    else
        pick = ok1 ? 1 : ok0 ? 0 : -1;
    if (pick == 0 && !img_read(st_path[0]))
        pick = -1;
    if (pick < 0)
        clear_store();
    me_store.magic = ST_MAGIC;
    me_store.nrec = NREC;
    return 1;
}

// 0 busy, 1 saved, -1 failed.
static s32 store_save(void)
{
    ekfile f;
    u32 ino, seq = me_store.seq + 1;
    s32 r, w;
    if (!ek_ready())
        return 0;
    EK_LOCK(EK_MUTEX);
    r = EK_LOOKUP("/cfw", &ino, 0, 0, 0);
    EK_UNLOCK(EK_MUTEX);
    if (r == NOT_FOUND && EK_MKDIR("/cfw") != 0)
        return -1;
    me_store.magic = ST_MAGIC;
    me_store.nrec = NREC;
    me_store.seq = seq;
    me_store.sum = file_sum();
    if (EK_OPEN(st_path[seq & 1], "w", &f) != 0)
        return -1;
    w = EK_WRITE(&me_store, sizeof me_store, &f);
    EK_CLOSE(&f);
    return w == (s32)sizeof me_store ? 1 : -1;
}

// ---- the firmware's project save and load (store.s) --------------------------
void me_project_saving(const char *name)
{
    copy_name(st_saving, name);
    st_saving[NAME_LEN] = 0;
    st_save_req = 1;
}

void me_project_loading(const char *name)
{
    copy_name(st_loading, name);
    st_loading[NAME_LEN] = 0;
    st_load_req = 1;
}

// ---- ev_tick ------------------------------------------------------------------
void me_store_tick(void)
{
    if (!st_loaded) {
        if (!store_load())
            return;                           // the drive is not ready yet
        st_loaded = 1;
        me_store_loads++;
        if (me_store.work.used)               // power-on: as it was at power-off
            apply(&me_store.work);
        st_last = cfg_sum();
    }
    if (st_save_req) {                        // the project is being saved: so are these
        st_save_req = 0;
        snapshot(place(st_saving), st_saving);
        snapshot(&me_store.work, st_saving);
        st_dirty = 1;
        st_wait = 0;
    }
    if (st_load_req) {                        // a project is being loaded: its settings
        st_load_req = 0;
        struct st_rec *r = find(st_loading);
        if (r)
            apply(r);
        else
            defaults();
        snapshot(&me_store.work, st_loading);
        st_last = cfg_sum();
        st_dirty = 1;
        st_wait = 0;
    }
    u32 now = cfg_sum();
    if (now != st_last) {                     // a change on the page: the working record
        st_last = now;
        snapshot(&me_store.work, me_store.work.name);
        st_dirty = 1;
        st_wait = SAVE_WAIT;
    }
    if (!st_dirty)
        return;
    if (st_wait > 0) {
        st_wait--;
        return;
    }
    switch (store_save()) {
    case 1:
        st_dirty = 0;
        me_store_saves++;
        break;
    case -1:
        me_store_fails++;
        st_wait = RETRY;
        break;
    default:                                  // busy: the next tick
        break;
    }
}
