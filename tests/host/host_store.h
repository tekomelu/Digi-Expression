// Stand-ins for the firmware's ekFS calls store.c uses: files and folders in
// memory, by path. "w" on an existing file writes over it from the start and
// keeps its size if the new data is shorter (as the firmware's: Digi Slicer).
#include <string.h>
typedef struct { u32 inode, pos, writable, open; } ekfile;
#define H_FILES 400
#define H_FILE_MAX 8192
static struct { char path[64]; int dir; s32 len; uint8_t data[H_FILE_MAX]; } h_fs[H_FILES];
static int h_nfs, h_mounted = 1, h_owner, h_write_fail, h_open_count, h_max_open, h_ops;
static int h_find(const char *p) { for (int i = 0; i < h_nfs; i++) if (!strcmp(h_fs[i].path, p)) return i; return -1; }
static int h_parent_ok(const char *p)
{
    char d[64]; strncpy(d, p, 63); d[63] = 0;
    char *s = strrchr(d, '/');
    if (!s || s == d) return 1;
    *s = 0;
    int i = h_find(d);
    return i >= 0 && h_fs[i].dir;
}
static s32 h_ek_open(const char *p, const char *mode, ekfile *f)
{
    int i = h_find(p);
    h_ops++;
    if (mode[0] == 'r' && (i < 0 || h_fs[i].dir)) return 1;
    if (mode[0] == 'w' && i < 0) {
        if (!h_parent_ok(p) || h_nfs >= H_FILES) return 1;
        i = h_nfs++; strncpy(h_fs[i].path, p, 63); h_fs[i].dir = 0; h_fs[i].len = 0;
    }
    f->inode = (u32)i; f->pos = 0; f->writable = mode[0] == 'w'; f->open = 1;
    if (++h_open_count > h_max_open) h_max_open = h_open_count;
    return 0;
}
static s32 h_ek_read(void *b, s32 n, ekfile *f)
{
    s32 len = h_fs[f->inode].len;
    if (n > len) n = len;
    memcpy(b, h_fs[f->inode].data, (size_t)n);
    return n;
}
static s32 h_ek_write(const void *b, s32 n, ekfile *f)
{
    if (h_write_fail) { if (h_write_fail > 1) { memcpy(h_fs[f->inode].data, b, (size_t)n / 2); } return -1; }
    if (n > H_FILE_MAX) return -1;
    memcpy(h_fs[f->inode].data, b, (size_t)n);
    if (n > h_fs[f->inode].len) h_fs[f->inode].len = n;
    return n;
}
static s32 h_ek_close(ekfile *f) { f->open = 0; h_open_count--; return 0; }
static s32 h_ek_mkdir(const char *p)
{
    if (h_find(p) >= 0 || !h_parent_ok(p) || h_nfs >= H_FILES) return 1;
    int i = h_nfs++; strncpy(h_fs[i].path, p, 63); h_fs[i].dir = 1; h_fs[i].len = 0;
    return 0;
}
static s32 h_ek_lookup(const char *p, u32 *ino, s32 a, s32 b, s32 c) { (void)a; (void)b; (void)c; int i = h_find(p); *ino = (u32)i; return i >= 0 ? 0 : 0x19; }
static void h_ek_lock(void *m) { (void)m; }
// The text of a file, for checks (NUL-terminated copy).
static const char *h_ftext(const char *p) { static char t[H_FILE_MAX + 1]; int i = h_find(p); if (i < 0) return 0; memcpy(t, h_fs[i].data, (size_t)h_fs[i].len); t[h_fs[i].len] = 0; return t; }
static void h_put_text(const char *p, const char *text)
{
    int i = h_find(p);
    if (i < 0) { i = h_nfs++; strncpy(h_fs[i].path, p, 63); h_fs[i].dir = 0; }
    h_fs[i].len = (s32)strlen(text); memcpy(h_fs[i].data, text, (size_t)h_fs[i].len);
}
#define EK_OPEN    h_ek_open
#define EK_READ    h_ek_read
#define EK_WRITE   h_ek_write
#define EK_CLOSE   h_ek_close
#define EK_MKDIR   h_ek_mkdir
#define EK_LOOKUP  h_ek_lookup
#define EK_LOCK    h_ek_lock
#define EK_UNLOCK  h_ek_lock
#define EK_MUTEX   ((void *)0)
#define EK_OWNER   h_owner
#define EK_MOUNTED h_mounted
#define NOT_FOUND  0x19
