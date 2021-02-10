//
// i8274 / uPD7210 (based on Z80 SIO)
//
// Channel A: rs232
// Channel B: modem
//
// 3B1 uses "Status Affects Vector" with "Non-vectored" mode
//
// Logging into 3b1 via serial port:
//  3b1: Make sure getty is running (/etc/inittab)
//  Linux:
//   Use 'minicom -D ./serial-pty' (Ctrl-A + Q to quit)
// 	 Or 'picocom --omap delbs ./serial-pty' to connect (Ctrl-A + Ctrl-Q to quit)
//   Or 'putty -serial ./serial-pty' (use Shift-Backspace to backspace, or configure to send ^H)
//   Or 'screen ./serial-pty' (Ctrl-A + '\' to quit)
//
// File transfer:
//  minicom:
//    send: Ctrl-A + s, receive: Ctrl-A + r, select xmodem
//  picocom:
//    Use 'picocom --omap delbs --send-cmd "sx -vv" --receive-cmd "rx -vv" ./serial-pty'
//    Linux: rzsz (or lrzsz) package must be installed
//    send: Ctrl-A + Ctrl-S, receive: Ctrl-A + Ctrl-R
//  3b1:
//    send: 'umodem -sb <filename>'
//    receive: 'umodem -rb <filename>'
//
// Logging into linux machine via serial port:
//  3b1: Make sure getty is NOT running on 3b1 (/etc/inittab)
//  3b1: Add 'DIR tty000 0 9600' to /usr/lib/uucp/L-devices
//  Linux: Run 'agetty pts/? 9600 vt100' where pts/? is the PTY linked to by ./serial-pty
//  3b1: Use 'cu -l tty000' to connect out
//

#include "i8274.h"
#include <stdio.h>
#include <string.h>

//#define I8274_DEBUG
#ifndef I8274_DEBUG
#define NDEBUG
#endif
#include "utils.h"

#ifdef __linux__
// needed for PTY functions, unlink, symlink
#define __USE_XOPEN_EXTENDED

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#endif

#define SERIAL_PTY_FILENAME 		"serial-pty"

#define WR1_EXT_INT_ENABLE			0x01	// Ext/Status Int Enable
#define WR1_TX_INT_ENABLE			0x02	// TxInt Enable
#define WR1_STATUS_AFFECTS_VECTOR	0x04	// Status Affects Vector

#define WR1_RX_INT_ENABLE_MASK		0x18	// RxInt Enable Mask
#define WR1_RX_INT_DISABLE			0x00	// RxInt Disable
#define WR1_RX_INT_FIRST_CHAR		0x08	// RxInt on First Character
#define WR1_RX_INT_ALL_PARITY		0x10	// RxInt on All Characters (Parity error affects vector)
#define WR1_RX_INT_ALL				0x18	// RxInt on All Characters (Parity error does not affect vector)

#define WR2_VECTORED_INT_MODE		0x20	// Vectored Interrupt Mode

#define RR0_RX_CHAR_AVAILABLE		0x01	// Rx Character Available
#define RR0_INTERRUPT_PENDING		0x02	// Int Pending (Chan A only, always 0 in Chan B) - aka "Interrupt In-Service" -- turn on as soon as interrupt requested
#define RR0_TX_BUFFER_EMPTY			0x04	// Tx Buffer Empty
#define RR0_DCD						0x08	// Ext/Status Int - DCD* (Data Carrier Detect)
#define RR0_SYNC_HUNT				0x10	// Ext/Status Int - SYNDET* status in Async mode, always pulled high on 3b1
#define RR0_CTS						0x20	// Ext/Status Int - CTS* (Clear to Send)
#define RR0_TX_UNDERRUN				0x40	// Ext/Status Int - Tx Underrun/End of Message
#define RR0_BREAK					0x80	// Ext/Status Int - Break (Async mode: set when Break sequence (null char plus framing error) detected in data stream)

#define RR1_ALL_SENT				0x01	// All Sent
#define RR1_PARITY_ERROR			0x10	// Parity Error
#define RR1_RX_OVERRUN_ERROR		0x20	// Overrun Error
#define RR1_CRC_FRAMING_ERROR		0x40	// CRC/Framing Error

// 3b1 irq priority order
const char* IRQ_PRIORITY_STR[] = { "RxA", "TxA", "RxB", "TxB", "ExtA", "ExtB" };
enum i8274_IRQ_PRIORITY_ORDER {
	IRQ_RXA,	IRQ_TXA,
	IRQ_RXB,	IRQ_TXB,
	IRQ_EXTA,	IRQ_EXTB,
	IRQ_TOTAL
};

static bool fifo_empty(struct fifo *f) { return f->count == 0; }
static bool fifo_full(struct fifo *f) { return f->count == FIFOSIZE; }
static void fifo_reset(struct fifo *f) { f->count = f->head = f->tail = 0; }
static int fifo_remaining(struct fifo *f) { return (FIFOSIZE - f->count); }
static void fifo_put(struct fifo *f, uint8_t data)
{
  if (fifo_full(f)) return;
  f->buf[f->head++] = data;
  f->count++;
  if (f->head == FIFOSIZE) f->head = 0;
}
static uint8_t fifo_get(struct fifo *f)
{
  uint8_t data;

  if (fifo_empty(f)) return 0;
  data = f->buf[f->tail++];
  f->count--;
  if (f->tail == FIFOSIZE) f->tail = 0;
  return data;
}

// Set "Rx char available" flag and RxInt if data in fifo
static void check_rx_available(I8274_CTX *ctx, struct i8274_channel *chan)
{
	if (fifo_empty(&chan->rx_fifo)) {
		chan->rr[0] &= ~RR0_RX_CHAR_AVAILABLE;
		// need to turn off RxInt to make sure 3b1 doesn't try to read again once fifo is empty
		ctx->irq_request[(chan->id==CHAN_A) ? IRQ_RXA : IRQ_RXB] &= ~IRQ_REQUESTED;
	} else {
		chan->rr[0] |= RR0_RX_CHAR_AVAILABLE;

		if (chan->wr[1] & WR1_RX_INT_ENABLE_MASK) {  // RxInt enabled
			ctx->irq_request[(chan->id==CHAN_A) ? IRQ_RXA : IRQ_RXB] |= IRQ_REQUESTED;
			LOG("chan%c: **Rx IRQ (Char available) put in daisy chain", (chan->id==CHAN_A)?'A':'B');
		}
	}
}

// assumes 8086/88 Mode (V2V1V0) used by 3b1
static void set_vect_tx_buffer_empty(I8274_CTX *ctx, i8274_CHANNEL_INDEX chan_id)
{
	uint8_t rr2 = ctx->chanB.wr[2];

	rr2 &= ~0x07;
	if (chan_id == CHAN_A)
		rr2 |= 0x04;  // TxA
	else
		rr2 |= 0x00;  // TxB

	ctx->chanB.rr[2] = rr2;
}

// assumes 8086/88 Mode (V2V1V0) used by 3b1
static void set_vect_rx_char_received(I8274_CTX *ctx, i8274_CHANNEL_INDEX chan_id)
{
	uint8_t rr2 = ctx->chanB.wr[2];

	rr2 &= ~0x07;
	if (chan_id == CHAN_A)
		rr2 |= 0x06;  // RxA
	else
		rr2 |= 0x02;  // RxB
	ctx->chanB.rr[2] = rr2;
}

// assumes 8086/88 Mode (V2V1V0) used by 3b1
static void set_vect_no_int_pending(I8274_CTX *ctx)
{
	ctx->chanB.rr[2] = ctx->chanB.wr[2];
	ctx->chanB.rr[2] |= 0x07;
}

static void clear_irq_requests(I8274_CTX *ctx, i8274_CHANNEL_INDEX chan_id)
{
	if (chan_id == CHAN_A)
		ctx->irq_request[IRQ_RXA] = ctx->irq_request[IRQ_TXA] = ctx->irq_request[IRQ_EXTA] = IRQ_NONE;
	else
		ctx->irq_request[IRQ_RXB] = ctx->irq_request[IRQ_TXB] = ctx->irq_request[IRQ_EXTB] = IRQ_NONE;
}

static void interrupt_ack(I8274_CTX *ctx)
{
	int i;

	// Find highest priority requested irq
	for (i=0; i < IRQ_TOTAL; i++) {
		if (ctx->irq_request[i] & IRQ_REQUESTED) break;
	}
	// Acknowldge interrupt about to be serviced
	if (i < IRQ_TOTAL) {
		LOG("acknowledging irq: %s", IRQ_PRIORITY_STR[i]);
		ctx->irq_request[i] |= IRQ_ACCEPTED;
	} else {
		fprintf(stderr, "ERROR (interrupt_ack): no irq to acknowledge\n");
	}
}

static void end_of_interrupt(I8274_CTX *ctx)
{
	int i;

	// Find highest priority accepted irq
	for (i=0; i < IRQ_TOTAL; i++) {
		if (ctx->irq_request[i] & IRQ_ACCEPTED) break;
	}
	// Disable highest priority irq just serviced
	if (i < IRQ_TOTAL) {
		LOG("disabling serviced irq: %s", IRQ_PRIORITY_STR[i]);
		ctx->irq_request[i] &= ~IRQ_ACCEPTED;
	} else {
		fprintf(stderr, "ERROR (EOI): can't find interrupt to disable\n");
	}
	// should we clear INT Pending bit? probably can rely on i8274_get_irq() for setting that
}

static void pty_in(I8274_CTX *ctx)
{
#ifdef __linux__
	uint8_t inbuf[FIFOSIZE], *byteptr;
	int bytes_read;

	// fill up FIFO with data from PTY
	while (!fifo_full(&ctx->chanA.rx_fifo) &&
		   (bytes_read = read(ctx->ptyfd, inbuf, fifo_remaining(&ctx->chanA.rx_fifo))) > 0) {
		byteptr = inbuf;
		while(bytes_read--)
			fifo_put(&ctx->chanA.rx_fifo, *(byteptr++));
	}

	// set "Rx char available" flag and RxInt if needed
	check_rx_available(ctx, &ctx->chanA);
#endif
}

// any new data incoming from serial PTY?
// called from the 60Hz update
void i8274_scan_incoming(I8274_CTX *ctx, i8274_CHANNEL_INDEX chan_id)
{
	if (chan_id == CHAN_A) pty_in(ctx);
}

bool i8274_get_irq(I8274_CTX *ctx)
{
	int i;

	// Look for any interrupts under service
	for (i=0; i < IRQ_TOTAL; i++) {
		if (ctx->irq_request[i] & IRQ_ACCEPTED) {
			// IRQ under service, don't change anything
			LOG("IRQ under service: %s", IRQ_PRIORITY_STR[i]);
			return true;
		}
	}

	// If nothing under service, find highest priority requested irq
	for (i=0; i < IRQ_TOTAL; i++) {
		if (ctx->irq_request[i] & IRQ_REQUESTED) break;
	}
	// Assumes WR1_STATUS_AFFECTS_VECTOR is set
	// Set the interrupt vector, will be read in rr[2] to ack and start ISR
    switch (i) {
		case IRQ_RXA:
			set_vect_rx_char_received(ctx, CHAN_A);
			ctx->chanA.rr[0] |= RR0_INTERRUPT_PENDING;
			LOG("**requesting m68k interrupt: RxA, vect: %x", ctx->chanB.rr[2]);
			return true;
			break;
		case IRQ_TXA:
			set_vect_tx_buffer_empty(ctx, CHAN_A);
			ctx->chanA.rr[0] |= RR0_INTERRUPT_PENDING;
			LOG("**requesting m68k interrupt: TxA, vect: %x", ctx->chanB.rr[2]);
			return true;
			break;
		case IRQ_RXB:
			set_vect_rx_char_received(ctx, CHAN_B);
			ctx->chanA.rr[0] |= RR0_INTERRUPT_PENDING;
			LOG("**requesting m68k interrupt: RxB, vect: %x", ctx->chanB.rr[2]);
			return true;
			break;
		case IRQ_TXB:
			set_vect_tx_buffer_empty(ctx, CHAN_B);
			ctx->chanA.rr[0] |= RR0_INTERRUPT_PENDING;
			LOG("**requesting m68k interrupt: TxB, vect: %x", ctx->chanB.rr[2]);
			return true;
			break;
		case IRQ_EXTA:
		case IRQ_EXTB:
			LOGS("ExtA, ExtB -- unsupported IRQ");
			// not implemented - fall through
		case IRQ_TOTAL:
		default:
			set_vect_no_int_pending(ctx);
			ctx->chanA.rr[0] &= ~RR0_INTERRUPT_PENDING;
			return false;
			break;
	}
}

// Resets latched status bits of RR0, INT prioritization logic, and all control regs (WR0-WR7)
static void channel_reset(I8274_CTX *ctx, i8274_CHANNEL_INDEX chan_id)
{
	struct i8274_channel *chan = (chan_id==CHAN_A) ? &ctx->chanA : &ctx->chanB;
	
	fifo_reset(&chan->rx_fifo);

	// clear write registers
	for (int i=0; i < 8; i++) {
		chan->wr[i] = 0;
	}

	// set no interrupts pending
	clear_irq_requests(ctx, chan_id);

	chan->rr[0] &= ~RR0_RX_CHAR_AVAILABLE;
	chan->rr[0] |= RR0_TX_BUFFER_EMPTY;
	chan->rr[0] |= RR0_TX_UNDERRUN;

	chan->rr[0] &= ~RR0_SYNC_HUNT;  // SYNDET is pulled high on 3b1 for Chan A and Chan B, so always reads OFF

	// Chan A: always set DCD and CTS to indicate DCD and CTS are coming from terminal/PTY
	if (chan_id == CHAN_A)
	{
		chan->rr[0] |= RR0_DCD;
		chan->rr[0] |= RR0_CTS;
	}
	// Chan B: DCD should reflect RI input, CTS should reflect DSR input, disable them for now (as they could auto-enable Rx/Tx if WR3:D5 set)
	if (chan_id == CHAN_B)
	{
		chan->rr[0] &= ~RR0_DCD;
		chan->rr[0] &= ~RR0_CTS;
	}

	chan->rr[1] &= ~(RR1_PARITY_ERROR | RR1_CRC_FRAMING_ERROR | RR1_RX_OVERRUN_ERROR);
	chan->rr[1] |= RR1_ALL_SENT;
}

#ifdef I8274_DEBUG
static void log_read_register(struct i8274_channel *chan, uint8_t read_reg, uint8_t value)
{
	printf("chan%c: <<<< read %02X from RR%i:", 'A'+chan->id, value, read_reg);
	switch (read_reg) {
		case 0: //RR0
			printf(" [%sRx Char Available]", (value & RR0_RX_CHAR_AVAILABLE) ? "" : "No ");
			printf(" [%sInt Pending]", (value & RR0_INTERRUPT_PENDING) ? "" : "No ");
			if (value & RR0_TX_BUFFER_EMPTY) printf(" [Tx Buffer Empty]");
			printf(" [DCD: %s]", (value & RR0_DCD) ? "ON" : "OFF");
			printf(" [SYNDET: %s]", (value & RR0_SYNC_HUNT) ? "ON" : "OFF");
			printf(" [CTS: %s]", (value & RR0_CTS) ? "ON" : "OFF");
			if (value & RR0_TX_UNDERRUN) printf(" [Tx Underrun]");
			if (value & RR0_BREAK) printf(" [Break]");
			break;

		case 1: //RR1
			if (value & RR1_ALL_SENT) printf(" [All sent]");
			printf(" [Residue Code: %02X]", (value >> 1) & 7);
			if (value & RR1_PARITY_ERROR) printf(" [Parity Error]");
			if (value & RR1_RX_OVERRUN_ERROR) printf(" [Rx Overrun Error]");
			if (value & RR1_CRC_FRAMING_ERROR) printf(" [CRC/Framing Error]");
			if ((value & (RR1_PARITY_ERROR | RR1_CRC_FRAMING_ERROR | RR1_RX_OVERRUN_ERROR)) == 0) printf(" [No Errors]");
			if (value & 0x80) printf(" [End of Frame (SDLC)]");
			break;

		case 2: //RR2 - Chan B Int Vector
			// If the "status affects vector" mode is selected (WR1:D2),
			// it contains the modified vector for the highest priority interrupt pending.
			// If no interrupts are pending, the variable bits in the vector are set to ones.
			if (chan->id == CHAN_B) printf(" [INT ACK][Interrupt Vector %02X]", value);
			break;
	}
	printf("\n");
}

static void log_write_register(struct i8274_channel *chan, uint8_t write_reg, uint8_t value)
{
	printf("chan%c: write %02X to WR%i:", 'A'+chan->id, value, write_reg);
	switch (write_reg) {
		case 1: //WR1
			// Changes to DCD, CTS, SYNDET generate interrupt
			printf(" [External/Status Interrupt: %s]", (value & WR1_EXT_INT_ENABLE) ? "Enabled" : "Disabled");
			// Interrupt when Tx buffer empty
			printf(" [TxInt: %s]", (value & WR1_TX_INT_ENABLE) ? "Enabled" : "Disabled");
			// Bit 2 inactive in channel A
			if (chan->id == CHAN_B && (value & WR1_STATUS_AFFECTS_VECTOR)) printf(" [Status Affects Vector]");
			// Character Rx handling
			switch ((value >> 3) & 3) {
				case 0: printf(" [RxInt: Disabled]"); break;
				case 1: printf(" [RxInt On First Char Only]"); break;
				case 2: printf(" [RxInt On All Received Chars (with parity error)]"); break;
				case 3: printf(" [RxInt On All Received Chars]"); break;
			}
			if (value & 0x20) printf(" [Wait on Rx]");
			if (value & 0x40) printf(" [Tx Byte Count Enable]");
			if (value & 0x80) printf(" [Wait on Rx/Tx Enable]");
			break;

		case 2: //WR2
			if (chan->id == CHAN_B) { // channel B
				printf(" [Interrupt Vector: %02X]", value);
			} else { // channel A
				if (value & 1) printf(" [Chan A: DMA, Chan B: Interrupt]");
				if (value & 2) printf(" [Chan A/Chan B: DMA]");
				if ((value & 3) == 0) printf(" [Chan A/Chan B: Interrupt]");
				printf(" [Relative Priority: %s]", (value & 0x04) ? "RxA, RxB, TxA, TxB" : "RxA, TxA, RxB, TxB");
				printf(" [%s Interrupt]", (value & WR2_VECTORED_INT_MODE) ? "Vectored" : "Non-vectored");
				if ((value & 0x18) == 0x10)
					printf(" [8086/88 Mode (V2V1V0)]");
				else
					printf(" [8085 Mode (V4V3V2)]");
				printf(" [Chan B Pin 10 = %s]", (value & 0x80) ? "SYNDET" : "RTS");  // pin 10 (Chan B RTS*) is always high
			}
			break;

		case 3: //WR3
			printf(" [Receiver: %s]", (value & 1) ? "*Enable*" : "Disable");
			if (value & 2) printf(" [Sync Char Load Inhibit]");
			if (value & 4) printf(" [Address Search Mode]");
			if (value & 8) printf(" [Rx CRC Enable]");
			if (value & 0x10) printf(" [Enter Hunt Mode]");
			if ((value & 0x1e) == 0) printf(" [Async Mode]");  // bits D1-D4 all zero in async mode
			// auto enable Tx when CTS, auto enable Rx when DCD [CTS/DCD being set for chan A in channel_reset(), may not want that]
			if (value & 0x20) printf(" [Auto Enable (DCD->Rx, CTS->Tx)]");
			printf(" [Rx Bits/Char: %c]", "5768"[value >> 6]);
			break;

		case 4: //WR4
			printf(" [Parity: %s]", (value & 1) ? "Enabled" : "Disabled");
			if (value & 1) printf(" [Parity: %s]", (value & 2) ? "Even" : "Odd");
			switch ((value >> 2) & 3) {
				case 0: printf(" [Sync Mode]"); break;
				case 1: printf(" [Async Mode, 1 Stop Bit]"); break;
				case 2: printf(" [Async Mode, 1.5 Stop Bits]"); break;
				case 3: printf(" [Async Mode, 2 Stop Bits]"); break;
			}
			switch ((value >> 4) & 3) {
				case 0: printf(" [8-Bit Sync Char]"); break;
				case 1: printf(" [16-Bit Sync Char]"); break;
				case 2: printf(" [SDLC/HDLC]"); break;
				case 3: printf(" [Ext Sync (SYNC pin)]"); break;
			}
			// clock rate of 19.2k
			switch ((value >> 6) & 3) {  // tx/rx clock rate
				case 0: printf(" [Data Rate = Clock Rate]"); break;
				case 1: printf(" [Data Rate = 1/16 Clock Rate = 1200 baud]"); break;
				case 2: printf(" [Data Rate = 1/32 Clock Rate]"); break;
				case 3: printf(" [Data Rate = 1/64 Clock Rate = 300 baud]"); break;
			}
			break;

		case 5: //WR5
			if (value & 1) printf(" [Tx CRC Enable]");
			printf(" [RTS pin: %s]", (value & 2) ? "ON" : "OFF");
			if (value & 4) printf(" [CRC-16]");
			if ((value & 5) == 0) printf(" [Async Mode]");  // 0 in bit 0 and 2 likely async mode
			printf(" [Transmitter: %s]", (value & 8) ? "*Enable*" : "Disable");
			if (value & 0x10) printf(" [Send Break]");
			printf(" [Tx Bits/Char: %c]", "5768"[(value >> 5) & 3]);
			if (chan->id == CHAN_A)
				printf(" [DTR pin: %s]", (value & 0x80) ? "ON" : "OFF");  // connects to DCD
			else 	// Chan B: DTR used to select clock for Chan A
				printf(" [rs232 clock: %s]", (value & 0x80) ? "int baud gen TMOUT" : "ext rs232 clock");
			break;

		case 6: //WR6
			printf(" [Sync Byte 1: %02X]", value);
			break;

		case 7: //WR7
			printf(" [Sync Byte 2: %02X]", value);
			break;
	}
	printf("\n");
}
#endif

// Rx: 3b1 receiving char from serial port (PTY)
uint8_t i8274_data_in(I8274_CTX *ctx, i8274_CHANNEL_INDEX chan_id)
{
	uint8_t data;
	struct i8274_channel *chan = (chan_id==CHAN_A) ? &ctx->chanA : &ctx->chanB;

	if (fifo_empty(&chan->rx_fifo)) {
		data = 0;
		fprintf(stderr, "chan%c: ERROR - Rx fifo empty!\n", 'A'+chan_id);
	} else {
		data = fifo_get(&chan->rx_fifo);
		LOG("chan%c: data in <<< 0x%02X ('%c')", 'A'+chan_id, data, data);
	}

	// ISR is started with RxInt but will continue to read more data depending on the RR0_RX_CHAR_AVAILABLE flag
	// (and will also read RR1 to make sure there are no errors)
	// i.e. ISR will potentially read more than just one byte (unlike Tx which seems to want one TxInt per byte)
	// check_rx_available() will set RR0_RX_CHAR_AVAILABLE accordingly and continue to request RxInt or turn it off
	check_rx_available(ctx, chan);
	return data;
}

static void pty_out(I8274_CTX *ctx, uint8_t byte_out)
{
#ifdef __linux__
	(void) write(ctx->ptyfd, &byte_out, 1);
#endif
}

// Tx: 3b1 sending char out serial port (PTY)
void i8274_data_out(I8274_CTX *ctx, i8274_CHANNEL_INDEX chan_id, uint8_t data)
{
	struct i8274_channel *chan = (chan_id==CHAN_A) ? &ctx->chanA : &ctx->chanB;
	LOG("chan%c: data out >>> 0x%02X ('%c')", 'A'+chan_id, data, data);

	// we immediately "process" the byte (send to PTY) so we can continue to say we are "buffer empty"
	if (chan_id == CHAN_A) pty_out(ctx, data);
	chan->rr[0] |= RR0_TX_BUFFER_EMPTY;

	// TODO: maybe do this TxInt Request in the 60Hz update so we aren't always immediately spamming TxInt back to the 3b1?
	// If enabled, need to request TxInt when Tx buffer is empty
	if ((chan->wr[1] & WR1_TX_INT_ENABLE) && (chan->rr[0] & RR0_TX_BUFFER_EMPTY)) {
		ctx->irq_request[(chan_id==CHAN_A) ? IRQ_TXA : IRQ_TXB] |= IRQ_REQUESTED;
		LOG("chan%c: **Tx IRQ (Tx buffer empty) put in daisy chain", 'A'+chan_id);
	}
}

// read from RR0-RR2
// RR1 read is used to check for any Rx errors
// RR2, Chan B read is used to get interrupt vector bits and call ISR
uint8_t i8274_status_read(I8274_CTX *ctx, i8274_CHANNEL_INDEX chan_id)
{
	uint8_t regptr;
	struct i8274_channel *chan = (chan_id==CHAN_A) ? &ctx->chanA : &ctx->chanB;

	LOG("chan%c: ctrl in", 'A'+chan_id);
	regptr = chan->wr[0] & 0x07;
	chan->wr[0] &= ~0x07;
#ifdef I8274_DEBUG
	log_read_register(chan, regptr, chan->rr[regptr]);
#endif
	// Interrupt Acknowledged with Chan B RR2 read (getting the interrupt vector bits)
	if (chan_id == CHAN_B && regptr == 2) {
		interrupt_ack(ctx);

		// MAME: "in non-vectored mode this serves the same function as the end of the second acknowledge cycle"
		// end_of_interrupt() can be called here, but since 3b1 writes WR0 EOI at the end of ISR we'll just call end_of_interrupt() there
#if 0
		if ((ctx->chanA.wr[2] & WR2_VECTORED_INT_MODE) == 0) {  //non-vectored mode
			end_of_interrupt(ctx);
		}
#endif
	}

	return chan->rr[regptr];
}

// write to WR0-WR7
void i8274_control_write(I8274_CTX *ctx, i8274_CHANNEL_INDEX chan_id, uint8_t data)
{
	uint8_t regptr;
	struct i8274_channel *chan = (chan_id==CHAN_A) ? &ctx->chanA : &ctx->chanB;

	LOG("chan%c: ctrl out %02X", 'A'+chan_id, data);
	regptr = chan->wr[0] & 0x07;
	chan->wr[0] &= ~0x07;
	chan->wr[regptr] = data;

	if (regptr == 0) {
		int cmd = (data >> 3) & 0x07;
		switch (cmd) {
			// WR0 Null
			case 0:
				break;

			// WR0 Send Abort
			case 1:
				LOG("chan%c: WR0 cmd: SDLC send abort", 'A'+chan_id);
				break;

			// WR0 Reset Ext/Status Interrupts
			// resets the latched status bits of RR0 and re-enables them, allowing interrupts to occur again
			case 2:
				LOG("chan%c: WR0 cmd: Reset ext/status interrupts", 'A'+chan_id);
				// supposed to relatch DCD and CTS here
				ctx->irq_request[(chan_id==CHAN_A) ? IRQ_EXTA : IRQ_EXTB] &= ~IRQ_REQUESTED;
				break;

			// WR0 Channel Reset
			case 3:
				LOG("chan%c: WR0 cmd: Channel reset", 'A'+chan_id);
				channel_reset(ctx, chan_id);
				break;

			// WR0 Enable INT on Next Rx Character
			// if INT on First Rx char mode selected, reactivate that mode after each complete msg received to prepare for next msg
			case 4:
				LOG("chan%c: WR0 cmd: Enable INT on next Rx char", 'A'+chan_id);
				break;

			// WR0 Reset TxINT Pending
			// 3b1 sends this cmd when last character sent
			// when no more chars to be sent, this cmd prevents further TxInt requests until next char completely sent
			case 5:
				LOG("chan%c: WR0 cmd: Reset TxINT pending", 'A'+chan_id);
				ctx->irq_request[(chan_id==CHAN_A) ? IRQ_TXA : IRQ_TXB] &= ~IRQ_REQUESTED;
				break;

			// WR0 Error Reset
			case 6:
				LOG("chan%c: WR0 cmd: Error reset", 'A'+chan_id);
				chan->rr[1] &= ~(RR1_PARITY_ERROR | RR1_CRC_FRAMING_ERROR | RR1_RX_OVERRUN_ERROR);
				break;

			// WR0 End of Interrupt (received on Chan A only, but applies to both channels)
			// 8274: "resets the interrupt-in-service latch of the highest-priority internal device under service"
			// 7201: "once an IRQ has been issued by 7201, all lower priority interrupts in the daisy chain are
			//        held off to permit the current interrupt to be serviced while allowing higher priority interrupts
			//        to occur. At some point in ISR (generally at the end), EOI cmd must be issued to Chan A to reenable
			//        the daisy chain and allow any pending lower priority internal interrupts to occur."
			case 7:
				LOG("chan%c: WR0 cmd: End of Interrupt", 'A'+chan_id);
				// MAME treats this as a NOP for 8274, and does EOI at the same time as ACK in the Chan B RR2 read
				// but since 3b1 calls WR0 EOI at end of ISR, we'll just disable IRQ here as that seems more appropriate
				if (chan_id == CHAN_A) {
					end_of_interrupt(ctx);
				}
				break;
		}
		uint8_t crc_reset_code = (data & 0xC0) >> 6;
		switch (crc_reset_code) {
			case 0:
				break;
			case 1:
				LOG("chan%c: reset Rx CRC Checker", 'A'+chan_id);
				break;
			case 2:
				LOG("chan%c: reset Tx CRC Generator", 'A'+chan_id);
				break;
			case 3:
				LOG("chan%c: reset Tx Underrun/End of Message Latch", 'A'+chan_id);
				chan->rr[0] &= ~RR0_TX_UNDERRUN;
				break;
		}
	}
#ifdef I8274_DEBUG
	if (regptr) log_write_register(chan, regptr, data);
#endif
}

#ifdef __linux__
static void ttySetRaw(int fd)
{
	struct termios t;

	tcgetattr(fd, &t);

	// Noncanonical mode - disable: signals, extended input processing, echoing
	t.c_lflag &= ~(ICANON | ISIG | IEXTEN | ECHO);

	// Disable special handling of CR, NL, and BREAK
	// No 8th-bit stripping or parity error handling
	// Disable START/STOP output flow control
	t.c_iflag &= ~(BRKINT | ICRNL | IGNBRK | IGNCR | INLCR | INPCK | ISTRIP | IXON | PARMRK);

	// Disable all output processing
	t.c_oflag &= ~OPOST;

	// Non-blocking
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;

	tcsetattr(fd, TCSAFLUSH, &t);
}
#endif

static void pty_init(I8274_CTX *ctx)
{
#ifdef __linux__	
	ctx->ptyfd = open("/dev/ptmx", O_RDWR | O_NONBLOCK);
	(void)grantpt(ctx->ptyfd);
	(void)unlockpt(ctx->ptyfd);
	ttySetRaw(ctx->ptyfd);
	unlink(SERIAL_PTY_FILENAME);
	if (symlink(ptsname(ctx->ptyfd), SERIAL_PTY_FILENAME) == 0)
		printf("Serial port (tty000) on pty %s\n", ptsname(ctx->ptyfd));
	else
		fprintf(stderr, "Error symlinking to pty: %s\n", strerror(errno));
#endif
}

static void pty_done(I8274_CTX *ctx)
{
#ifdef __linux__
	close(ctx->ptyfd);
	unlink(SERIAL_PTY_FILENAME);
#endif	
}

void i8274_init(I8274_CTX *ctx)
{
	memset(&ctx->chanA, 0, sizeof(ctx->chanA));
	memset(&ctx->chanB, 0, sizeof(ctx->chanB));
	ctx->chanA.id = CHAN_A;
	ctx->chanB.id = CHAN_B;
	channel_reset(ctx, CHAN_A);
	channel_reset(ctx, CHAN_B);
	pty_init(ctx);
}

void i8274_done(I8274_CTX *ctx)
{
	pty_done(ctx);
}
