#ifndef _TC8250_H
#define _TC8250_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
	bool chip_enable;
	bool address_latch_enable;
	bool write_enable;
	uint8_t address;
	uint8_t seconds_offset;
	uint8_t minutes_offset;
	uint8_t hours_offset;
	uint8_t days_offset;
	uint8_t months_offset;
	uint8_t years_offset;
	uint8_t weekday_offset;
} TC8250_CTX;

void tc8250_init(TC8250_CTX *ctx);
void tc8250_set_chip_enable(TC8250_CTX *ctx, bool enabled);
void tc8250_set_address_latch_enable(TC8250_CTX *ctx, bool enabled);
void tc8250_set_write_enable(TC8250_CTX *ctx, bool enabled);
uint8_t tc8250_read_reg(TC8250_CTX *ctx);
void tc8250_write_reg(TC8250_CTX *ctx, uint8_t val);

enum {
	ONE_SEC_DIGT  = 0x0, /* 1 sec digit */
	TEN_SEC_DIGT  = 0x1, /* 10 sec digit */
	ONE_MIN_DIGT  = 0x2, /* 1 minute digit */
	TEN_MIN_DIGT  = 0x3, /* 10 minutes digit */
	ONE_HR_DIGT   = 0x4, /* 1 hour digit */
	TEN_HR_DIGT   = 0x5, /* 10 hours digit */
	ONE_DAY_DIGT  = 0x6, /* 1 day digit */
	TEN_DAY_DIGT  = 0x7, /* 10 days digit */
	ONE_MNTH_DIGT = 0x8, /* 1 month digit */
	TEN_MNTH_DIGT = 0x9, /* 10 month digit */
	ONE_YR_DIGT   = 0xa, /* 1 year digit */
	TEN_YR_DIGT   = 0xb, /* 10 year digit */
	WEEK_DAY      = 0xc, /* day of the week */
	TOUT_CONTROL  = 0xd, /* Tout control */
	PROTECT_KEY   = 0xe, /* protection key */
	RTC_STATUS    = 0xf  /* real time clock status */
};
#endif
