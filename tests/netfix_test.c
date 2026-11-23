/*
 * Tests for src/netfix.c: make test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../src/netfix.h"

static int failed;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s\n", what); failed++; } else printf("ok: %s\n", what); } while (0)

// a frame from 10.0.2.2:sport to 10.0.2.15:dport, with seq, flags and data
static int frame(uint8_t *f, uint16_t sport, uint16_t dport, uint32_t seq, uint8_t flags, const void *data, int n)
{
	memset(f, 0, 14 + 40 + n);
	f[12] = 0x08;
	uint8_t *ip = f + 14, *tcp = ip + 20;
	ip[0] = 0x45;
	ip[2] = (40 + n) >> 8; ip[3] = (40 + n) & 0xff;
	ip[9] = 6;
	ip[12] = 10; ip[14] = 2; ip[15] = 2;
	ip[16] = 10; ip[18] = 2; ip[19] = 15;
	tcp[0] = sport >> 8; tcp[1] = sport & 0xff;
	tcp[2] = dport >> 8; tcp[3] = dport & 0xff;
	tcp[4] = seq >> 24; tcp[5] = seq >> 16; tcp[6] = seq >> 8; tcp[7] = seq;
	tcp[12] = 5 << 4;
	tcp[13] = flags;
	memcpy(tcp + 20, data, n);
	return 14 + 40 + n;
}

// the TCP checksum is right
static int sum_ok(const uint8_t *f, int n)
{
	const uint8_t *ip = f + 14, *tcp = ip + 20;
	int const tcp_len = 20 + n;
	uint32_t sum = 6 + tcp_len;
	for (int i = 12; i < 20; i += 2)
		sum += (ip[i] << 8) | ip[i + 1];
	for (int i = 0; i + 1 < tcp_len; i += 2)
		sum += (tcp[i] << 8) | tcp[i + 1];
	if (tcp_len & 1)
		sum += tcp[tcp_len - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return sum == 0xffff;
}

static uint8_t f[1600];
#define DATA(f) ((f) + 54)

// send a segment; the data afterwards is in DATA(f)
static int seg(uint16_t sport, uint16_t dport, uint32_t seq, const void *d, int n)
{
	int const len = frame(f, sport, dport, seq, 0x10, d, n);
	netfix_frame(f, len);
	return len;
}

static void syn(uint16_t sport, uint16_t dport, uint32_t seq)
{
	int const len = frame(f, sport, dport, seq, 0x12, "", 0);
	netfix_frame(f, len);
}

int main(void)
{
	static const uint8_t will_status[] = { 255, 251, 5 };
	static const uint8_t wont_status[] = { 255, 252, 5 };

	netfix_config(false, false);
	syn(23, 1025, 100);
	seg(23, 1025, 101, will_status, 3);
	CHECK(!memcmp(DATA(f), will_status, 3), "off: nothing changes");

	netfix_config(true, true);
	syn(23, 1025, 100);
	seg(23, 1025, 101, will_status, 3);
	CHECK(!memcmp(DATA(f), wont_status, 3), "telnet: IAC WILL STATUS becomes WONT");
	CHECK(sum_ok(f, 3), "telnet: the checksum is right");
	{
		static const uint8_t d[] = { 255, 253, 5 }, e[] = { 255, 254, 5 };
		seg(23, 1025, 104, d, 3);
		CHECK(!memcmp(DATA(f), e, 3), "telnet: IAC DO STATUS becomes DONT");
	}
	{
		// IAC IAC is the data byte 255, here split over two segments:
		// then 251 5 are data and must stay
		static const uint8_t a[] = { 'x', 255 }, b[] = { 255, 251, 5 };
		seg(23, 1025, 107, a, 2);
		seg(23, 1025, 109, b, 3);
		CHECK(!memcmp(DATA(f), b, 3), "telnet: IAC IAC split over segments is data");
	}
	{
		// IAC at the end of one segment, WILL STATUS in the next
		static const uint8_t a[] = { 'y', 255 }, b[] = { 251, 5 }, e[] = { 252, 5 };
		seg(23, 1025, 112, a, 2);
		seg(23, 1025, 114, b, 2);
		CHECK(!memcmp(DATA(f), e, 2), "telnet: IAC | WILL STATUS over segments");
	}
	{
		// the last segment again: the same change
		static const uint8_t b[] = { 251, 5 }, e[] = { 252, 5 };
		seg(23, 1025, 114, b, 2);
		CHECK(!memcmp(DATA(f), e, 2), "telnet: a segment sent again gets the same change");
	}
	seg(23, 1025, 500, will_status, 3);
	CHECK(!memcmp(DATA(f), will_status, 3), "telnet: out of order stays");
	seg(23, 1025, 116, "ab", 2);
	seg(23, 1025, 118, will_status, 3);
	CHECK(!memcmp(DATA(f), wont_status, 3), "telnet: in order again after that");
	{
		// another port: not telnet
		syn(24, 1026, 0);
		seg(24, 1026, 1, will_status, 3);
		CHECK(!memcmp(DATA(f), will_status, 3), "telnet: only from port 23");
	}
	{
		// a stream netfix did not see start: left alone
		seg(23, 1027, 1, will_status, 3);
		CHECK(!memcmp(DATA(f), will_status, 3), "telnet: an unknown stream stays");
	}

	const char *port = "PORT 127,0,0,1,4,1\r\n", *fixed = "PORT 10,0,2,02,4,1\r\n";
	int const pl = (int)strlen(port);
	syn(1030, 21, 1000);
	seg(1030, 21, 1001, port, pl);
	CHECK(!memcmp(DATA(f), fixed, pl), "ftp: PORT 127,0,0,1 at a line start changes");
	CHECK(sum_ok(f, pl), "ftp: the checksum is right");
	{
		const char *d = "STOR PORT 127,0,0,1,\r\n";
		int const n = (int)strlen(d);
		seg(1030, 21, 1001 + pl, d, n);
		CHECK(!memcmp(DATA(f), d, n), "ftp: not in the middle of a line");
		// the next line starts in the next segment
		seg(1030, 21, 1001 + pl + n, port, pl);
		CHECK(!memcmp(DATA(f), fixed, pl), "ftp: a line that starts a segment");
	}
	{
		// a line start at the end of the last segment, "PO" then the rest
		syn(1031, 21, 0);
		seg(1031, 21, 1, "PO", 2);
		seg(1031, 21, 3, port + 2, pl - 2);
		CHECK(!memcmp(DATA(f), port + 2, pl - 2), "ftp: a PORT split over segments stays whole");
		seg(1031, 21, 1 + pl, port, pl);
		CHECK(!memcmp(DATA(f), fixed, pl), "ftp: the next line changes");
	}
	{
		syn(21, 1032, 0);
		seg(21, 1032, 1, port, pl);
		CHECK(!memcmp(DATA(f), port, pl), "ftp: only to port 21");
	}
	printf(failed ? "%d FAILED\n" : "all passed\n", failed);
	return failed != 0;
}
