#pragma once
/*
 * Small GIF87a/89a decoder for the slideshow: the whole file sits in memory,
 * frames are composited onto an RGB565 canvas (transparency, disposal
 * methods 0-3, interlacing, local/global palettes). Loops forever.
 * Not thread-safe per decoder; use one decoder from one task at a time.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gif gif_t;

/* Parses the header. data must stay valid until gif_close(). NULL if not a
 * GIF, too large (> 4096 px a side or 4 Mpx) or out of memory. The canvas is
 * the logical screen, grown to fit any frame that sticks out of it. */
gif_t *gif_open(const uint8_t *data, size_t size);
int gif_width(const gif_t *g);
int gif_height(const gif_t *g);

/* Decodes the next frame onto the canvas.
 *  1  new frame; *delay_ms = how long to show it (min 20 ms, 100 ms if unset)
 *  0  still image: the one frame is already on the canvas, nothing changed
 * -1  broken file */
int gif_next_frame(gif_t *g, int *delay_ms);

/* gif_width() x gif_height() RGB565 pixels, row-major. */
const uint16_t *gif_canvas(const gif_t *g);

/* The canvas rectangle the last gif_next_frame() == 1 changed (w/h 0 if
 * nothing), so only that part needs redrawing. */
void gif_dirty(const gif_t *g, int *x, int *y, int *w, int *h);

void gif_close(gif_t *g);

#ifdef __cplusplus
}
#endif
