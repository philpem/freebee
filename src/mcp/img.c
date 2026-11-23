/*
 * img - PNG files and AVI video (ZMBV + PCM) for the MCP server.
 *
 * The AVI and ZMBV writers follow LEMU's ui/avi.c (GPL-2.0-or-later): a
 * plain RIFF AVI with an idx1 index, as ffmpeg writes one, whose headers
 * are rewritten now and then so a file cut short still plays. ZMBV is
 * DOSBox's lossless screen codec; ffmpeg, VLC and video editors read it.
 * Here it runs in its 8-bit palette format, which suits a 1-bit screen.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#include "img.h"

/* ---- PNG ---- */

static void be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void png_chunk(sbuf *out, const char *type, const uint8_t *data, uint32_t len)
{
	uint8_t hdr[8], crc[4];
	be32(hdr, len);
	memcpy(hdr + 4, type, 4);
	uLong c = crc32(0, hdr + 4, 4);
	if (len)
		c = crc32(c, data, len);
	be32(crc, c);
	sb_add(out, hdr, 8);
	if (len)
		sb_add(out, data, len);
	sb_add(out, crc, 4);
}

bool png_encode(sbuf *out, int w, int h, const uint8_t *pix, int stride,
		const uint8_t *pal, int ncolors)
{
	size_t rowlen = (size_t)w + 1;
	size_t rawlen = rowlen * h;
	uint8_t *raw = malloc(rawlen);
	if (!raw)
		return false;
	for (int y = 0; y < h; y++) {
		raw[y * rowlen] = 0;	// filter: none
		memcpy(raw + y * rowlen + 1, pix + (size_t)y * stride, w);
	}
	uLongf zlen = compressBound(rawlen);
	uint8_t *z = malloc(zlen);
	if (!z || compress2(z, &zlen, raw, rawlen, 6) != Z_OK) {
		free(raw);
		free(z);
		return false;
	}
	free(raw);
	static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	uint8_t ihdr[13];
	be32(ihdr, w);
	be32(ihdr + 4, h);
	ihdr[8] = 8;				// bit depth
	ihdr[9] = pal ? 3 : 0;		// palette or grey
	ihdr[10] = ihdr[11] = ihdr[12] = 0;
	sb_add(out, sig, 8);
	png_chunk(out, "IHDR", ihdr, 13);
	if (pal)
		png_chunk(out, "PLTE", pal, ncolors * 3);
	png_chunk(out, "IDAT", z, zlen);
	png_chunk(out, "IEND", NULL, 0);
	free(z);
	return true;
}

/* ---- AVI ---- */

#define AVIIF_KEYFRAME      0x10
#define AVIF_HASINDEX       0x10
#define AVIF_ISINTERLEAVED  0x100
#define AVIF_TRUSTCKTYPE    0x800

typedef struct {
	uint32_t fourcc, flags, offset, size;
} avi_index;

struct avi_file {
	FILE *f;
	char *error;
	int fps, audio_rate, channels;
	long pos;			// where the next chunk goes
	long movi;			// the 'movi' of LIST movi
	avi_index *index;
	size_t n_index, cap_index;
	uint32_t video_frames, audio_frames;
	uint32_t video_max, audio_max;
	// header fields that change, by their offset in the file
	long riff_size, total_frames, buffer_size;
	long video_length, video_buffer;
	long audio_length, audio_buffer;
	long movi_size;
};

static void le32(sbuf *b, uint32_t v)
{
	uint8_t p[4] = { v, v >> 8, v >> 16, v >> 24 };
	sb_add(b, p, 4);
}

static void le16(sbuf *b, uint16_t v)
{
	uint8_t p[2] = { v, v >> 8 };
	sb_add(b, p, 2);
}

static void put4cc(sbuf *b, const char *cc)
{
	sb_add(b, cc, 4);
}

static long list_begin(sbuf *b, const char *cc, const char *type)
{
	put4cc(b, cc);
	long at = b->len;
	le32(b, 0);
	if (type)
		put4cc(b, type);
	return at;
}

static void list_end(sbuf *b, long at)
{
	uint32_t v = b->len - at - 4;
	uint8_t p[4] = { v, v >> 8, v >> 16, v >> 24 };
	memcpy(b->s + at, p, 4);
}

static void avi_fail(avi_file *a, const char *what)
{
	if (!a->error) {
		sbuf b;
		sb_init(&b);
		sb_printf(&b, "%s: %s", what, strerror(errno));
		a->error = sb_take(&b);
	}
}

static bool avi_patch(avi_file *a, long at, uint32_t v)
{
	uint8_t p[4] = { v, v >> 8, v >> 16, v >> 24 };
	if (fseek(a->f, at, SEEK_SET) || fwrite(p, 4, 1, a->f) != 1) {
		avi_fail(a, "can't update the video file");
		return false;
	}
	return true;
}

avi_file *avi_create(const char *path, int width, int height, int fps,
		int audio_rate, int channels, const char **err)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) {
		*err = strerror(errno);
		return NULL;
	}
	avi_file *a = calloc(1, sizeof(*a));
	a->f = fdopen(fd, "wb");
	if (!a->f) {
		*err = strerror(errno);
		close(fd);
		free(a);
		return NULL;
	}
	a->fps = fps;
	a->audio_rate = audio_rate;
	a->channels = channels;

	sbuf h;
	sb_init(&h);
	long riff = list_begin(&h, "RIFF", "AVI ");
	a->riff_size = riff;
	long hdrl = list_begin(&h, "LIST", "hdrl");

	long chunk = list_begin(&h, "avih", NULL);
	le32(&h, 1000000 / fps);		// microseconds a frame
	le32(&h, 0);					// max bytes a second
	le32(&h, 0);					// padding granularity
	le32(&h, AVIF_HASINDEX | AVIF_ISINTERLEAVED | AVIF_TRUSTCKTYPE);
	a->total_frames = h.len;
	le32(&h, 0);
	le32(&h, 0);					// initial frames
	le32(&h, audio_rate ? 2 : 1);	// streams
	a->buffer_size = h.len;
	le32(&h, 0);
	le32(&h, width);
	le32(&h, height);
	le32(&h, 0); le32(&h, 0); le32(&h, 0); le32(&h, 0);
	list_end(&h, chunk);

	long strl = list_begin(&h, "LIST", "strl");
	chunk = list_begin(&h, "strh", NULL);
	put4cc(&h, "vids");
	put4cc(&h, "ZMBV");
	le32(&h, 0);					// flags
	le16(&h, 0);					// priority
	le16(&h, 0);					// language
	le32(&h, 0);					// initial frames
	le32(&h, 1);					// scale
	le32(&h, fps);					// rate
	le32(&h, 0);					// start
	a->video_length = h.len;
	le32(&h, 0);
	a->video_buffer = h.len;
	le32(&h, 0);
	le32(&h, 0xffffffff);			// quality
	le32(&h, 0);					// sample size
	le16(&h, 0); le16(&h, 0);
	le16(&h, width); le16(&h, height);
	list_end(&h, chunk);
	long strf = list_begin(&h, "strf", NULL);	// BITMAPINFOHEADER
	le32(&h, 40);
	le32(&h, width);
	le32(&h, height);
	le16(&h, 1);					// planes
	le16(&h, 24);					// bits, as DOSBox has it
	put4cc(&h, "ZMBV");
	le32(&h, width * height * 3);
	le32(&h, 0); le32(&h, 0); le32(&h, 0); le32(&h, 0);
	list_end(&h, strf);
	list_end(&h, strl);

	if (audio_rate) {
		int block = 2 * channels;
		strl = list_begin(&h, "LIST", "strl");
		chunk = list_begin(&h, "strh", NULL);
		put4cc(&h, "auds");
		le32(&h, 1);
		le32(&h, 0);
		le16(&h, 0); le16(&h, 0);
		le32(&h, 0);
		le32(&h, block);			// scale: bytes a frame
		le32(&h, audio_rate * block);	// rate: bytes a second
		le32(&h, 0);
		a->audio_length = h.len;
		le32(&h, 0);
		a->audio_buffer = h.len;
		le32(&h, 0);
		le32(&h, 0xffffffff);
		le32(&h, block);			// sample size
		le16(&h, 0); le16(&h, 0); le16(&h, 0); le16(&h, 0);
		list_end(&h, chunk);
		strf = list_begin(&h, "strf", NULL);	// WAVEFORMATEX
		le16(&h, 1);				// PCM
		le16(&h, channels);
		le32(&h, audio_rate);
		le32(&h, audio_rate * block);
		le16(&h, block);			// block align
		le16(&h, 16);				// bits
		list_end(&h, strf);
		list_end(&h, strl);
	}
	list_end(&h, hdrl);

	a->movi_size = list_begin(&h, "LIST", "movi");
	a->movi = h.len - 4;
	list_end(&h, a->movi_size);
	list_end(&h, riff);

	if (fwrite(h.s, h.len, 1, a->f) != 1) {
		*err = strerror(errno);
		fclose(a->f);
		unlink(path);
		sb_free(&h);
		free(a);
		return NULL;
	}
	a->pos = h.len;
	sb_free(&h);
	return a;
}

static bool avi_chunk(avi_file *a, const char *cc, const void *data, size_t len, uint32_t flags)
{
	static const uint8_t pad;
	uint8_t head[8];
	if (a->error)
		return false;
	memcpy(head, cc, 4);
	head[4] = len; head[5] = len >> 8; head[6] = len >> 16; head[7] = len >> 24;
	if (fwrite(head, 8, 1, a->f) != 1 ||
	    (len && fwrite(data, len, 1, a->f) != 1) ||
	    ((len & 1) && fwrite(&pad, 1, 1, a->f) != 1)) {
		avi_fail(a, "can't write the video file");
		return false;
	}
	if (a->n_index == a->cap_index) {
		size_t cap = a->cap_index ? a->cap_index * 2 : 4096;
		avi_index *ix = realloc(a->index, cap * sizeof(*ix));
		if (!ix) {
			avi_fail(a, "out of memory");
			return false;
		}
		a->index = ix;
		a->cap_index = cap;
	}
	avi_index *e = &a->index[a->n_index++];
	e->fourcc = cc[0] | (cc[1] << 8) | (cc[2] << 16) | ((uint32_t)cc[3] << 24);
	e->flags = flags;
	e->offset = a->pos - a->movi;
	e->size = len;
	a->pos += 8 + len + (len & 1);
	return true;
}

bool avi_add_video(avi_file *a, const void *data, size_t len, bool key)
{
	if (!avi_chunk(a, "00dc", data, len, key ? AVIIF_KEYFRAME : 0))
		return false;
	a->video_frames++;
	if (len > a->video_max)
		a->video_max = len;
	return true;
}

bool avi_add_audio(avi_file *a, const int16_t *samples, size_t frames)
{
	size_t n = frames * a->channels;
	size_t len = n * 2;
	if (!frames)
		return true;
	uint8_t *le = malloc(len);
	if (!le) {
		avi_fail(a, "out of memory");
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		le[2 * i] = samples[i];
		le[2 * i + 1] = (uint16_t)samples[i] >> 8;
	}
	bool ok = avi_chunk(a, "01wb", le, len, AVIIF_KEYFRAME);
	free(le);
	if (!ok)
		return false;
	a->audio_frames += frames;
	if (len > a->audio_max)
		a->audio_max = len;
	return true;
}

static bool avi_headers(avi_file *a, long end)
{
	uint32_t maxbuf = a->video_max > a->audio_max ? a->video_max : a->audio_max;
	bool ok = avi_patch(a, a->riff_size, end - 8) &&
		avi_patch(a, a->total_frames, a->video_frames) &&
		avi_patch(a, a->buffer_size, maxbuf) &&
		avi_patch(a, a->video_length, a->video_frames) &&
		avi_patch(a, a->video_buffer, a->video_max) &&
		(!a->audio_rate ||
		 (avi_patch(a, a->audio_length, a->audio_frames) &&
		  avi_patch(a, a->audio_buffer, a->audio_max))) &&
		avi_patch(a, a->movi_size, a->pos - a->movi);
	if (ok && fseek(a->f, end, SEEK_SET)) {
		avi_fail(a, "can't write the video file");
		ok = false;
	}
	return ok;
}

bool avi_sync(avi_file *a)
{
	if (a->error)
		return false;
	if (!avi_headers(a, a->pos) || fflush(a->f)) {
		avi_fail(a, "can't write the video file");
		return false;
	}
	return true;
}

bool avi_close(avi_file *a, char **err)
{
	if (!a->error) {
		sbuf idx;
		sb_init(&idx);
		put4cc(&idx, "idx1");
		le32(&idx, a->n_index * 16);
		for (size_t i = 0; i < a->n_index; i++) {
			le32(&idx, a->index[i].fourcc);
			le32(&idx, a->index[i].flags);
			le32(&idx, a->index[i].offset);
			le32(&idx, a->index[i].size);
		}
		long end = a->pos + idx.len;
		if (fwrite(idx.s, idx.len, 1, a->f) != 1)
			avi_fail(a, "can't write the video index");
		else
			avi_headers(a, end);
		sb_free(&idx);
	}
	if (fclose(a->f) && !a->error)
		avi_fail(a, "can't write the video file");
	bool ok = !a->error;
	if (err)
		*err = a->error;
	else
		free(a->error);
	free(a->index);
	free(a);
	return ok;
}

uint64_t avi_bytes(avi_file *a)
{
	return a->pos;
}

/* ---- ZMBV, 8-bit palette ---- */

#define ZMBV_KEYFRAME	1
#define ZMBV_DELTAPAL	2
#define ZMBV_FMT_8BPP	4
#define ZMBV_BLOCK		16

struct zmbv {
	int width, height, bx, by;
	size_t table;		// the motion vector table, padded to 4
	z_stream zs;
	uint8_t *prev;		// the last frame
	uint8_t pal[768];
	sbuf work;
};

zmbv *zmbv_new(int width, int height, int level)
{
	zmbv *z = calloc(1, sizeof(*z));
	if (!z)
		return NULL;
	z->width = width;
	z->height = height;
	z->bx = (width + ZMBV_BLOCK - 1) / ZMBV_BLOCK;
	z->by = (height + ZMBV_BLOCK - 1) / ZMBV_BLOCK;
	z->table = ((size_t)z->bx * z->by * 2 + 3) & ~(size_t)3;
	z->prev = calloc(1, (size_t)width * height);
	sb_init(&z->work);
	if (!z->prev || deflateInit(&z->zs, level) != Z_OK) {
		free(z->prev);
		free(z);
		return NULL;
	}
	return z;
}

void zmbv_free(zmbv *z)
{
	if (!z)
		return;
	deflateEnd(&z->zs);
	free(z->prev);
	sb_free(&z->work);
	free(z);
}

static bool zmbv_deflate(zmbv *z, const uint8_t *in, size_t len, sbuf *out)
{
	z->zs.next_in = (Bytef *)in;
	z->zs.avail_in = len;
	do {
		size_t room = (len / 8 > 4096 ? len / 8 : 4096) + 64;
		z->zs.next_out = (Bytef *)sb_reserve(out, room);
		z->zs.avail_out = room;
		int ret = deflate(&z->zs, Z_SYNC_FLUSH);
		if (ret != Z_OK && ret != Z_BUF_ERROR)
			return false;
		out->len += room - z->zs.avail_out;
	} while (z->zs.avail_out == 0 || z->zs.avail_in);
	return true;
}

bool zmbv_encode(zmbv *z, bool key, const uint8_t *frame, const uint8_t *pal, sbuf *out)
{
	int w = z->width, h = z->height;
	out->len = 0;
	if (key) {
		uint8_t hdr[7] = { ZMBV_KEYFRAME, 0, 1, 1, ZMBV_FMT_8BPP, ZMBV_BLOCK, ZMBV_BLOCK };
		if (deflateReset(&z->zs) != Z_OK)
			return false;
		sb_add(out, hdr, sizeof(hdr));
		z->work.len = 0;
		sb_add(&z->work, pal, 768);
		sb_add(&z->work, frame, (size_t)w * h);
		memcpy(z->prev, frame, (size_t)w * h);
		memcpy(z->pal, pal, 768);
		return zmbv_deflate(z, (uint8_t *)z->work.s, z->work.len, out);
	}

	uint8_t flags = 0;
	z->work.len = 0;
	if (memcmp(pal, z->pal, 768)) {
		flags |= ZMBV_DELTAPAL;
		uint8_t x[768];
		for (int i = 0; i < 768; i++)
			x[i] = pal[i] ^ z->pal[i];
		sb_add(&z->work, x, 768);
		memcpy(z->pal, pal, 768);
	}
	size_t tab = z->work.len;
	uint8_t *zero = calloc(1, z->table);
	if (!zero)
		return false;
	sb_add(&z->work, zero, z->table);
	free(zero);
	for (int j = 0; j < z->by; j++) {
		int py = j * ZMBV_BLOCK;
		int bh = h - py < ZMBV_BLOCK ? h - py : ZMBV_BLOCK;
		for (int i = 0; i < z->bx; i++) {
			int px = i * ZMBV_BLOCK;
			int bw = w - px < ZMBV_BLOCK ? w - px : ZMBV_BLOCK;
			int r;
			for (r = 0; r < bh; r++)
				if (memcmp(z->prev + (size_t)(py + r) * w + px,
					   frame + (size_t)(py + r) * w + px, bw))
					break;
			if (r == bh)
				continue;
			z->work.s[tab + ((size_t)j * z->bx + i) * 2] = 1;
			for (r = 0; r < bh; r++) {
				uint8_t row[ZMBV_BLOCK];
				uint8_t *p = z->prev + (size_t)(py + r) * w + px;
				const uint8_t *c = frame + (size_t)(py + r) * w + px;
				for (int k = 0; k < bw; k++)
					row[k] = p[k] ^ c[k];
				sb_add(&z->work, row, bw);
				memcpy(p, c, bw);
			}
		}
	}
	sb_add(out, &flags, 1);
	return zmbv_deflate(z, (uint8_t *)z->work.s, z->work.len, out);
}
