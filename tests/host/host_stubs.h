// Stand-ins for the OS 1.54 addresses expr.c uses, for host unit tests.
#include <stdint.h>

extern volatile int32_t h_auto_channel, h_active_track, h_track_channel[16], h_receive_cc;
extern volatile uint32_t h_output_to;
extern volatile int32_t h_tx_pending;
extern volatile uint32_t h_tx_base, h_tx_sent;
extern volatile uint16_t h_dma_a, h_dma_b;

int32_t h_channel(int32_t midi_track);
void h_din_send(int32_t len, const uint8_t *bytes);
void h_stock_cc(const uint8_t *msg, int32_t len, int32_t flag);

#define AUTO_CHANNEL   h_auto_channel
#define ACTIVE_TRACK   h_active_track
#define TRACK_CHANNEL  h_track_channel
#define RECEIVE_CC     h_receive_cc
#define OUTPUT_TO      h_output_to
#define TX_PENDING     h_tx_pending
#define TX_BASE        h_tx_base
#define TX_SENT        h_tx_sent
#define DMA37_A        h_dma_a
#define DMA37_B        h_dma_b
#define TX_RING        4096
#define CHANNEL        h_channel
#define DIN_SEND       h_din_send
#define STOCK_CC       h_stock_cc

extern volatile int8_t h_params[8 * 106];
#define PARAMS         h_params
