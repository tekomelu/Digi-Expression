// Stand-ins for the OS 1.54 addresses expr.c uses, for host unit tests.
#include <stdint.h>

extern volatile int32_t h_auto_channel, h_active_track, h_track_channel[16], h_receive_cc;
extern volatile uint32_t h_output_to;
extern volatile uint8_t h_notes[1 + 1024], h_ccs[1 + 1024];

int32_t h_channel(int32_t midi_track);
void h_queue_send(void *queue, void *item);
void h_stock_cc(const uint8_t *msg, int32_t len, int32_t flag);

#define AUTO_CHANNEL   h_auto_channel
#define ACTIVE_TRACK   h_active_track
#define TRACK_CHANNEL  h_track_channel
#define RECEIVE_CC     h_receive_cc
#define OUTPUT_TO      h_output_to
#define TASK_QUEUE     ((void *)0)
#define NOTES_LEN      h_notes[0]
#define NOTES_BUF      (h_notes + 1)
#define CCS_LEN        h_ccs[0]
#define CCS_BUF        (h_ccs + 1)
#define CHANNEL        h_channel
#define QUEUE_SEND     h_queue_send
#define STOCK_CC       h_stock_cc

extern volatile int8_t h_params[8 * 106];
#define PARAMS         h_params
