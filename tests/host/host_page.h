// Stand-ins for the OS 1.54 addresses page.c uses, for host unit tests.
#include <stdint.h>
extern volatile int32_t h_active_track;
extern int h_trig_page, h_func_held, h_push_held, h_draw_calls, h_knob_calls;
void h_text(void *bmp, const void *font, int x, int y, int colour, const char *fmt, ...);
void h_fillrect(void *bmp, int x0, int y0, int x1, int y1, int colour);
#define ACTIVE_TRACK_P h_active_track
#define TEXT       h_text
#define FILLRECT   h_fillrect
#define FONT5      ((const void *)0)
#define LABEL_FONT ((const void *)0)
#define TITLE_FONT ((const void *)0)
#define KEYHELD(k) ((k) == 1 ? h_func_held : (k) >= 40 ? h_push_held : 0)
#define KNOB_GFX(o, id, v, a, b, bmp, x, y) ((void)(v), h_knob_calls++)
#define STOCK_NAME(a, sid) ((void)(sid), "Name")
static void *trig_page(void *ctrl) { (void)ctrl; return h_trig_page ? (void *)1 : 0; }
// The accelerator: 256 words a raw count, so a slow turn is one step a detent.
static int h_enc_last_sid, h_enc_zero;
static int enc_words(int k, int sid, int n, int pushed) { (void)k; h_enc_last_sid = sid; return h_enc_zero ? 0 : n * 256 * (pushed ? 4 : 1); }
static void enc_reset(void) {}
static void enc_decay(void) {}
