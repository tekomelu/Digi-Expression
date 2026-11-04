// Stand-ins for the OS 1.54 addresses page.c uses, for host unit tests.
#include <stdint.h>
extern volatile int32_t h_active_track;
extern int h_trig_page, h_func_held, h_draw_calls;
void h_text(void *bmp, const void *font, int x, int y, int colour, const char *fmt, ...);
void h_fillrect(void *bmp, int x0, int y0, int x1, int y1, int colour);
#define ACTIVE_TRACK_P h_active_track
#define TEXT       h_text
#define FILLRECT   h_fillrect
#define FONT5      ((const void *)0)
#define KEYHELD(k) ((k) == 1 ? h_func_held : 0)
static void *trig_page(void *ctrl) { (void)ctrl; return h_trig_page ? (void *)1 : 0; }
