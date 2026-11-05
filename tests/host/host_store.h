// Stand-ins for the firmware's ekFS calls store.c uses: two files in memory.
#include <string.h>
typedef struct { u32 inode, pos, writable, open; } ekfile;
#define H_FILE_MAX 40000
static uint8_t h_file[2][H_FILE_MAX];
static s32 h_file_len[2] = { -1, -1 };        // -1: no such file
static int h_dir, h_mounted = 1, h_owner, h_write_fail, h_opens;
static int h_which(const char *p) { return p[strlen(p) - 1] == 'a' ? 1 : 0; }
static s32 h_ek_open(const char *p, const char *mode, ekfile *f)
{
    int w = h_which(p);
    h_opens++;
    if (mode[0] == 'r' && h_file_len[w] < 0) return 1;
    if (mode[0] == 'w' && !h_dir) return 1;
    f->inode = (u32)w; f->pos = 0; f->writable = mode[0] == 'w'; f->open = 1;
    return 0;
}
static s32 h_ek_read(void *buf, s32 n, ekfile *f)
{
    s32 len = h_file_len[f->inode];
    if (n > len) n = len;
    memcpy(buf, h_file[f->inode], (size_t)n);
    return n;
}
static s32 h_ek_write(const void *buf, s32 n, ekfile *f)
{
    if (h_write_fail) return -1;
    memcpy(h_file[f->inode], buf, (size_t)n);
    h_file_len[f->inode] = n;
    return n;
}
static s32 h_ek_close(ekfile *f) { f->open = 0; return 0; }
static s32 h_ek_mkdir(const char *p) { (void)p; h_dir = 1; return 0; }
static s32 h_ek_lookup(const char *p, u32 *ino, s32 a, s32 b, s32 c) { (void)p; (void)a; (void)b; (void)c; *ino = 1; return h_dir ? 0 : 0x19; }
static void h_ek_lock(void *m) { (void)m; }
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
