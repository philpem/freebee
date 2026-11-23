/*
 * netfix - two optional fixes for 1986 network programs, in the frames
 * the host network sends to the 3B1. Both are off by default.
 *
 * - fix_telnet_status: telnet out of the 3B1. The 3B1's telnet client
 *   takes the WILL STATUS in a server's status report (RFC 859) for a new
 *   offer, and asks for the status again: with a telnetd that offers
 *   STATUS, as modern ones do, "STATUS IS :" fills the screen for ever. So
 *   from a server's port 23, IAC WILL STATUS becomes IAC WONT STATUS and
 *   IAC DO STATUS becomes IAC DONT STATUS, and the client never turns
 *   STATUS on.
 * - fix_ftp_port: FTP into the 3B1 through a forwarded port. The 3B1's
 *   ftpd has only active mode, and a client on the host's loopback tells
 *   it to connect back to 127.0.0.1, which on the 3B1 is the 3B1. So to
 *   the 3B1's port 21, a line that starts "PORT 127,0,0,1," starts
 *   "PORT 10,0,2,02," instead: 10.0.2.2 is the host as the 3B1 sees it.
 *
 * Each change keeps the length, so the TCP sequence numbers stay right;
 * then the TCP checksum is made again.
 *
 * TCP data is a byte stream, and a segment can end anywhere. So netfix
 * keeps the state of each stream (where in a telnet command, where in an
 * FTP line) from one segment to the next, with the sequence number the
 * next segment must have. A segment that does not start there (out of
 * order, or lost track) passes unchanged. A segment sent again from where
 * the last one started is done again from the state the last one started
 * with, so it gets the same change. A change whose first byte was in an
 * earlier segment is not made: that byte is gone.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "netfix.h"

#define TN_IAC		255
#define TN_WILL		251
#define TN_WONT		252
#define TN_DO		253
#define TN_DONT		254
#define TN_STATUS	5

static const char FTP_PORT_FROM[] = "PORT 127,0,0,1,";
static const char FTP_PORT_TO[]   = "PORT 10,0,2,02,";
#define FTP_PORT_LEN	(sizeof(FTP_PORT_FROM) - 1)

// The parse state at a point in a stream
typedef struct {
	// telnet: 0 data, 1 after IAC, 2 after IAC and a WILL/WONT/DO/DONT
	uint8_t tn;
	uint8_t tn_cmd;
	// ftp: at the start of a line; the bytes of FTP_PORT_FROM matched
	bool line_start;
	uint8_t matched;
} PARSE;

typedef struct {
	bool used;
	uint8_t src[4], dst[4];
	uint16_t sport, dport;
	uint32_t next_seq;		// where the next segment must start
	uint32_t last_seq;		// where the last one started
	PARSE last, now;		// the state there, and at next_seq
	unsigned age;
} STREAM;

#define STREAMS 16
static STREAM streams[STREAMS];
static unsigned clock_;
static bool fix_telnet, fix_ftp;

void netfix_config(bool telnet_status, bool ftp_port)
{
	fix_telnet = telnet_status;
	fix_ftp = ftp_port;
	netfix_reset();
}

void netfix_reset(void)
{
	memset(streams, 0, sizeof(streams));
}

static uint32_t get32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void tcp_new_checksum(uint8_t *ip, uint8_t *tcp, int tcp_len)
{
	uint32_t sum = 6 + tcp_len;		// pseudo header: protocol, length

	tcp[16] = tcp[17] = 0;
	for (int i = 12; i < 20; i += 2)	// source and destination address
		sum += (ip[i] << 8) | ip[i + 1];
	for (int i = 0; i + 1 < tcp_len; i += 2)
		sum += (tcp[i] << 8) | tcp[i + 1];
	if (tcp_len & 1)
		sum += tcp[tcp_len - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	sum = ~sum & 0xffff;
	tcp[16] = sum >> 8;
	tcp[17] = sum & 0xff;
}

static STREAM *find(const uint8_t *ip, uint16_t sport, uint16_t dport, bool make)
{
	STREAM *oldest = &streams[0];
	for (int i = 0; i < STREAMS; i++) {
		STREAM *s = &streams[i];
		if (s->used && s->sport == sport && s->dport == dport &&
				!memcmp(s->src, ip + 12, 4) && !memcmp(s->dst, ip + 16, 4))
			return s;
		if (!s->used || (oldest->used && s->age < oldest->age))
			oldest = s;
	}
	if (!make)
		return NULL;
	memset(oldest, 0, sizeof(*oldest));
	oldest->used = true;
	memcpy(oldest->src, ip + 12, 4);
	memcpy(oldest->dst, ip + 16, 4);
	oldest->sport = sport;
	oldest->dport = dport;
	return oldest;
}

// Go through n bytes of data from state p, changing them where a fix
// applies; true if a byte changed
static bool parse(PARSE *p, uint8_t *d, int n, bool telnet, bool ftp)
{
	bool changed = false;
	int cmd_at = -1;	// where the telnet command byte is in d, or -1

	for (int i = 0; i < n; i++) {
		uint8_t const c = d[i];
		if (telnet) {
			switch (p->tn) {
				case 0:
					if (c == TN_IAC)
						p->tn = 1;
					break;
				case 1:
					if (c >= TN_WILL && c <= TN_DONT) {
						p->tn = 2;
						p->tn_cmd = c;
						cmd_at = i;
					} else {
						p->tn = 0;	// IAC IAC is the data byte 255
					}
					break;
				default:
					if (c == TN_STATUS && (p->tn_cmd == TN_WILL || p->tn_cmd == TN_DO) && cmd_at >= 0) {
						d[cmd_at]++;	// WILL to WONT, DO to DONT
						changed = true;
					}
					p->tn = 0;
					cmd_at = -1;
					break;
			}
		}
		if (ftp) {
			if (p->matched > 0 && c == (uint8_t)FTP_PORT_FROM[p->matched]) {
				if (++p->matched == FTP_PORT_LEN) {
					// all of it is in this segment: change it
					if (i + 1 >= (int)FTP_PORT_LEN) {
						memcpy(d + i + 1 - FTP_PORT_LEN, FTP_PORT_TO, FTP_PORT_LEN);
						changed = true;
					}
					p->matched = 0;
				}
			} else if (p->line_start && c == (uint8_t)FTP_PORT_FROM[0]) {
				p->matched = 1;
			} else {
				p->matched = 0;
			}
			p->line_start = c == '\n';
		}
	}
	return changed;
}

void netfix_frame(uint8_t *f, int len)
{
	if (!fix_telnet && !fix_ftp)
		return;
	// IPv4, TCP, not a fragment
	if (len < 14 + 20 + 20 || f[12] != 0x08 || f[13] != 0x00)
		return;
	uint8_t *ip = f + 14;
	int const ihl = (ip[0] & 15) * 4;
	int const ip_len = (ip[2] << 8) | ip[3];
	if ((ip[0] >> 4) != 4 || ip[9] != 6 || ihl < 20 || ip_len > len - 14 || ip_len < ihl + 20)
		return;
	if ((((ip[6] << 8) | ip[7]) & 0x3fff) != 0)
		return;
	uint8_t *tcp = ip + ihl;
	int const tcp_len = ip_len - ihl;
	int const thl = (tcp[12] >> 4) * 4;
	if (thl < 20 || thl > tcp_len)
		return;
	uint16_t const sport = (tcp[0] << 8) | tcp[1];
	uint16_t const dport = (tcp[2] << 8) | tcp[3];
	bool const telnet = fix_telnet && sport == 23;
	bool const ftp = fix_ftp && dport == 21;
	if (!telnet && !ftp)
		return;

	uint8_t const flags = tcp[13];
	bool const syn = flags & 0x02, rst = flags & 0x04;
	uint32_t const seq = get32(tcp + 4);
	uint8_t *data = tcp + thl;
	int const n = tcp_len - thl;
	STREAM *s = find(ip, sport, dport, syn);
	if (s == NULL)
		return;			// a stream that started before: leave it
	s->age = ++clock_;
	if (rst) {
		s->used = false;
		return;
	}
	if (syn) {
		memset(&s->now, 0, sizeof(s->now));
		s->now.line_start = true;
		s->last = s->now;
		s->next_seq = s->last_seq = seq + 1;
		return;			// data on a SYN: not looked at
	}
	if (n == 0)
		return;

	PARSE p;
	if (seq == s->next_seq) {
		s->last = s->now;
		s->last_seq = seq;
		p = s->now;
	} else if (seq == s->last_seq) {
		p = s->last;		// the last segment again, maybe longer
	} else {
		return;			// out of order: unchanged
	}
	if (parse(&p, data, n, telnet, ftp))
		tcp_new_checksum(ip, tcp, tcp_len);
	s->now = p;
	s->next_seq = seq + n;
}
