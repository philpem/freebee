/*
 * img - PNG files and AVI video (ZMBV + PCM) for the MCP server.
 */
#ifndef _FB_IMG_H
#define _FB_IMG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "json.h"

/// Encode 8-bit pixels as a PNG into out (binary). With pal (ncolors RGB
/// triplets) the pixels are palette indexes, otherwise grey levels.
bool png_encode(sbuf *out, int w, int h, const uint8_t *pix, int stride,
		const uint8_t *pal, int ncolors);

/* ---- AVI with ZMBV video (8-bit palette) and 16-bit PCM audio ---- */

typedef struct avi_file avi_file;

/// Create path (it must not exist). audio_rate 0: no sound.
avi_file *avi_create(const char *path, int width, int height, int fps,
		int audio_rate, int channels, const char **err);
bool avi_add_video(avi_file *a, const void *data, size_t len, bool key);
bool avi_add_audio(avi_file *a, const int16_t *samples, size_t frames);
/// Rewrite the headers so the file plays even if it ends here.
bool avi_sync(avi_file *a);
bool avi_close(avi_file *a, char **err);
uint64_t avi_bytes(avi_file *a);

typedef struct zmbv zmbv;

zmbv *zmbv_new(int width, int height, int level);
void zmbv_free(zmbv *z);
/// Encode an 8-bit frame (width*height, palette indexes) into out (cleared
/// first). A key frame has the whole picture; otherwise only the blocks
/// that differ from the last frame. pal: 256 RGB triplets.
bool zmbv_encode(zmbv *z, bool key, const uint8_t *frame, const uint8_t *pal, sbuf *out);

#endif
