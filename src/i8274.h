#ifndef _I8274_H
#define _I8274_H

#include <stdint.h>
#include <stdbool.h>

#define FIFOSIZE	128

struct fifo {
	uint8_t buf[FIFOSIZE];
	int count, head, tail;
};

typedef enum {
	CHAN_A,
	CHAN_B
} i8274_CHANNEL_INDEX;

struct i8274_channel {
	uint8_t wr[8];
	uint8_t rr[3];
	i8274_CHANNEL_INDEX id;
	struct fifo rx_fifo;
};

typedef enum {
	IRQ_NONE,
	IRQ_REQUESTED,
	IRQ_ACCEPTED	// aka Z80 IEO ("Interrupt Enable Out")
} i8274_IRQ_STATUS;

typedef struct {
	struct i8274_channel chanA, chanB;
	
	i8274_IRQ_STATUS irq_request[6];

#ifdef __linux__
	int ptyfd;
#endif

} I8274_CTX;

void i8274_init(I8274_CTX *ctx);
void i8274_done(I8274_CTX *ctx);
bool i8274_get_irq(I8274_CTX *ctx);
void i8274_scan_incoming(I8274_CTX *ctx, i8274_CHANNEL_INDEX chan_id);

uint8_t i8274_status_read(I8274_CTX *ctx, i8274_CHANNEL_INDEX chan_id);
void i8274_control_write(I8274_CTX *ctx, i8274_CHANNEL_INDEX chan_id, uint8_t data);

uint8_t i8274_data_in(I8274_CTX *ctx, i8274_CHANNEL_INDEX chan_id);
void i8274_data_out(I8274_CTX *ctx, i8274_CHANNEL_INDEX chan_id, uint8_t data);

#endif


