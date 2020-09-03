#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include "tc8250.h"

#ifndef TC8250_DEBUG
#define NDEBUG
#endif
#include "utils.h"

void tc8250_init(TC8250_CTX *ctx)
{
	ctx->chip_enable = false;
	ctx->address_latch_enable = false;
	ctx->write_enable = false;
	ctx->address = 0;
}

void tc8250_set_chip_enable(TC8250_CTX *ctx, bool enabled)
{
	LOG("tc8250_set_chip_enable %d\n", enabled);
	ctx->chip_enable = enabled;
}

void tc8250_set_address_latch_enable(TC8250_CTX *ctx, bool enabled)
{
	LOG("tc8250_set_address_latch_enable %d\n", enabled);
	ctx->address_latch_enable = enabled;
}

void tc8250_set_write_enable(TC8250_CTX *ctx, bool enabled)
{
	LOG("tc8250_set_write_enable %d\n", enabled);
	ctx->write_enable = enabled;
}

uint8_t get_second(TC8250_CTX *ctx)
{
	time_t t;
	struct tm g;
	uint8_t ret;
	t = time(NULL);
	gmtime_r(&t, &g);
	ret = g.tm_sec;
	return (ret);
}

uint8_t get_minute(TC8250_CTX *ctx)
{
	time_t t;
	struct tm g;
	uint8_t ret;
	t = time(NULL);
	gmtime_r(&t, &g);
	ret = g.tm_min;
	return (ret);
}

uint8_t get_hour(TC8250_CTX *ctx)
{
	time_t t;
	struct tm g;
	uint8_t ret;
	t = time(NULL);
	gmtime_r(&t, &g);
	ret = g.tm_hour;
	return (ret);
}

uint8_t get_day(TC8250_CTX *ctx)
{
	time_t t;
	struct tm g;
	uint8_t ret;
	t = time(NULL);
	gmtime_r(&t, &g);
	ret = g.tm_mday;
	return (ret);
}

uint8_t get_month(TC8250_CTX *ctx)
{
	time_t t;
	struct tm g;
	uint8_t ret;
	t = time(NULL);
	gmtime_r(&t, &g);
	ret = g.tm_mon+1;
	return (ret);
}

uint8_t get_year(TC8250_CTX *ctx)
{
	/*time_t t;
	struct tm g;
	uint8_t ret;
	t = time(NULL);
	gmtime_r(&t, &g);
	ret = g.tm_year;
	return (ret);*/
	return (87);
}

uint8_t get_weekday(TC8250_CTX *ctx)
{
	time_t t;
	struct tm g;
	uint8_t ret;
	t = time(NULL);
	gmtime_r(&t, &g);
	ret = g.tm_wday;
	return (ret);
}

uint8_t tc8250_read_reg(TC8250_CTX *ctx)
{
	LOG("tc8250_read_reg %x\n", ctx->address);
	switch (ctx->address){
		case ONE_SEC_DIGT:
			return (get_second(ctx) % 10);
		case TEN_SEC_DIGT:
			return (get_second(ctx) / 10);
		case ONE_MIN_DIGT:
			return (get_minute(ctx) % 10);
		case TEN_MIN_DIGT:
			return (get_minute(ctx) / 10);
		case ONE_HR_DIGT:
			return (get_hour(ctx) % 10);
		case TEN_HR_DIGT:
			return (get_hour(ctx) / 10);
		case ONE_DAY_DIGT:
			return (get_day(ctx) % 10);
		case TEN_DAY_DIGT:
			return (get_day(ctx) / 10);
		case ONE_MNTH_DIGT:
			return (get_month(ctx) % 10);
		case TEN_MNTH_DIGT:
			return (get_month(ctx) / 10);
		case ONE_YR_DIGT:
			return (get_year(ctx) % 10);
		case TEN_YR_DIGT:
			return (get_year(ctx) / 10);
		case WEEK_DAY:
			return (get_weekday(ctx) / 10);
		case TOUT_CONTROL:
			return (0);
		case PROTECT_KEY:
			return (0);
		case RTC_STATUS:
			return (0);
		default:
			return (0);
	}
}

void set_seconds(TC8250_CTX *ctx, uint8_t val)
{
}

void set_minutes(TC8250_CTX *ctx, uint8_t val)
{
}

void set_hours(TC8250_CTX *ctx, uint8_t val)
{
}

void set_days(TC8250_CTX *ctx, uint8_t val)
{
}

void set_months(TC8250_CTX *ctx, uint8_t val)
{
}

void set_years(TC8250_CTX *ctx, uint8_t val)
{
}

void set_weekday(TC8250_CTX *ctx, uint8_t val)
{
}

void tc8250_write_reg(TC8250_CTX *ctx, uint8_t val)
{
	LOG("tc8250_write_reg %x", val);
	if (ctx->address_latch_enable){
		LOG(" address\n");
		ctx->address = val;
		return;
	}
	if (ctx->chip_enable){
		LOG(" %x\n", ctx->address);
		switch (ctx->address){
			case ONE_SEC_DIGT:
				set_seconds(ctx, val % 10);
				break;
			case TEN_SEC_DIGT:
				set_seconds(ctx, val % 10 * 10);
				break;
			case ONE_MIN_DIGT:
				set_minutes(ctx, val % 10);
				break;
			case TEN_MIN_DIGT:
				set_minutes(ctx, val % 10 * 10);
				break;
			case ONE_HR_DIGT:
				set_hours(ctx, val % 10);
				break;
			case TEN_HR_DIGT:
				set_hours(ctx, val % 10 * 10);
				break;
			case ONE_DAY_DIGT:
				set_days(ctx, val % 10);
				break;
			case TEN_DAY_DIGT:
				set_days(ctx, val % 10 * 10);
				break;
			case ONE_MNTH_DIGT:
				set_months(ctx, val % 10);
				break;
			case TEN_MNTH_DIGT:
				set_months(ctx, val % 10 * 10);
				break;
			case ONE_YR_DIGT:
				set_years(ctx, val % 10);
				break;
			case TEN_YR_DIGT:
				set_years(ctx, val % 10 * 10);
				break;
			case WEEK_DAY:
				set_weekday(ctx, val % 10);
				break;
			case TOUT_CONTROL:
				break;
			case PROTECT_KEY:
				break;
			case RTC_STATUS:
				break;
			default:
				break;
		}
	}else{
		LOG("\n");
	}
}
