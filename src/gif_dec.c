/*
 * GIF decoder (see gif_dec.h). LZW tables live in the decoder struct, so
 * nothing large goes on the (small) LVGL task stack.
 */
#include "gif_dec.h"
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"

#define MAX_SIDE   4096
#define MAX_PIXELS (4096u * 1024u)
#define LZW_MAX    4096

struct gif {
    const uint8_t *data;
    size_t size, pos;
    size_t frames_start;            /* first block after header + global palette */
    int w, h;
    uint16_t gpal[256];             /* global palette, RGB565 */
    bool has_gpal;
    uint16_t *canvas;
    uint16_t *backup;               /* for disposal 3, allocated on first use */

    /* graphic control extension for the coming frame */
    int delay_cs, transparent, disposal;
    /* the frame on the canvas now, for its disposal */
    int prev_disposal, px, py, pw, ph;
    int frames_in_pass;             /* frames decoded since the start of the stream */
    bool is_still;
    int dx0, dy0, dx1, dy1;         /* canvas area the last frame changed, [x0,x1) x [y0,y1) */

    /* LZW */
    uint16_t prefix[LZW_MAX];
    uint8_t suffix[LZW_MAX];
    uint8_t stack[LZW_MAX + 1];
    /* sub-block bit reader */
    size_t blk_left;
    uint32_t bitbuf;
    int bitcnt;
    bool data_end;
};

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static bool need(gif_t *g, size_t n)
{
    return g->pos + n <= g->size;
}

static int rd8(gif_t *g)
{
    return g->pos < g->size ? g->data[g->pos++] : -1;
}

static int rd16(gif_t *g)
{
    if (!need(g, 2)) {
        g->pos = g->size;
        return -1;
    }
    int v = g->data[g->pos] | (g->data[g->pos + 1] << 8);
    g->pos += 2;
    return v;
}

static bool read_palette(gif_t *g, uint16_t *pal, int n)
{
    if (!need(g, (size_t)n * 3)) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        const uint8_t *c = g->data + g->pos + i * 3;
        pal[i] = rgb565(c[0], c[1], c[2]);
    }
    for (int i = n; i < 256; i++) {
        pal[i] = 0;
    }
    g->pos += (size_t)n * 3;
    return true;
}

static bool skip_sub_blocks(gif_t *g)
{
    for (;;) {
        int n = rd8(g);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            return true;
        }
        if (!need(g, n)) {
            return false;
        }
        g->pos += n;
    }
}

/* ---- LZW bit reader over data sub-blocks ---- */
static int next_byte(gif_t *g)
{
    if (g->data_end) {
        return -1;
    }
    if (g->blk_left == 0) {
        int n = rd8(g);
        if (n <= 0) {
            g->data_end = true;                  /* block terminator (or EOF) */
            return -1;
        }
        g->blk_left = n;
    }
    g->blk_left--;
    return rd8(g);
}

static int read_code(gif_t *g, int size)
{
    while (g->bitcnt < size) {
        int b = next_byte(g);
        if (b < 0) {
            return -1;
        }
        g->bitbuf |= (uint32_t)b << g->bitcnt;
        g->bitcnt += 8;
    }
    int code = g->bitbuf & ((1u << size) - 1);
    g->bitbuf >>= size;
    g->bitcnt -= size;
    return code;
}

/* Decodes one image's LZW stream onto the canvas. */
static bool decode_image(gif_t *g, int fx, int fy, int fw, int fh, bool interlace, const uint16_t *pal)
{
    int min = rd8(g);
    if (min < 2 || min > 11) {
        return false;
    }
    g->blk_left = 0;
    g->bitbuf = 0;
    g->bitcnt = 0;
    g->data_end = false;

    const int clear = 1 << min, eoi = clear + 1;
    int size = min + 1, next = eoi + 1, old = -1;
    uint8_t first = 0;
    for (int i = 0; i < clear; i++) {
        g->prefix[i] = 0;
        g->suffix[i] = (uint8_t)i;
    }

    /* output position; interlaced rows go 0,8,16.. 4,12.. 2,6.. 1,3.. */
    static const uint8_t pass_start[4] = { 0, 4, 2, 1 }, pass_step[4] = { 8, 8, 4, 2 };
    int x = 0, y = 0, pass = 0;
    const long total = (long)fw * fh;
    long done = 0;
    const int trans = g->transparent;

    while (done < total) {
        int code = read_code(g, size);
        if (code < 0 || code == eoi) {
            break;
        }
        if (code == clear) {
            size = min + 1;
            next = eoi + 1;
            old = -1;
            continue;
        }
        int sp = 0, in = code;
        if (old < 0) {
            if (code >= clear) {
                return false;
            }
            g->stack[sp++] = (uint8_t)code;
            first = (uint8_t)code;
        } else {
            if (code > next) {
                return false;
            }
            if (code == next) {                         /* KwKwK */
                g->stack[sp++] = first;
                code = old;
            }
            while (code >= clear) {
                if (sp >= LZW_MAX) {
                    return false;
                }
                g->stack[sp++] = g->suffix[code];
                code = g->prefix[code];
            }
            first = (uint8_t)code;
            g->stack[sp++] = first;
            if (next < LZW_MAX) {
                g->prefix[next] = (uint16_t)old;
                g->suffix[next] = first;
                next++;
                if (next == (1 << size) && size < 12) {
                    size++;
                }
            }
        }
        old = in;
        /* emit (the stack holds the string backwards) */
        while (sp > 0 && done < total) {
            uint8_t idx = g->stack[--sp];
            int cx = fx + x, cy = fy + y;
            if (idx != trans && cx < g->w && cy < g->h) {
                g->canvas[cy * g->w + cx] = pal[idx];
            }
            done++;
            if (++x == fw) {
                x = 0;
                if (interlace) {
                    y += pass_step[pass];
                    while (y >= fh && pass < 3) {
                        pass++;
                        y = pass_start[pass];
                    }
                } else {
                    y++;
                }
            }
        }
    }
    /* skip what is left of the data (unused codes, trailing sub-blocks) */
    if (!g->data_end) {
        if (g->blk_left) {
            if (!need(g, g->blk_left)) {
                return false;
            }
            g->pos += g->blk_left;
            g->blk_left = 0;
        }
        if (!skip_sub_blocks(g)) {
            return false;
        }
    }
    return true;
}

static void dirty_add(gif_t *g, int x, int y, int w, int h)
{
    int x1 = x + w > g->w ? g->w : x + w, y1 = y + h > g->h ? g->h : y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= x1 || y >= y1) {
        return;
    }
    if (g->dx0 >= g->dx1) {                              /* empty so far */
        g->dx0 = x; g->dy0 = y; g->dx1 = x1; g->dy1 = y1;
        return;
    }
    if (x < g->dx0) g->dx0 = x;
    if (y < g->dy0) g->dy0 = y;
    if (x1 > g->dx1) g->dx1 = x1;
    if (y1 > g->dy1) g->dy1 = y1;
}

/* Some encoders declare a logical screen smaller than their frames (or 0x0).
 * Walk the blocks once (skipping the image data) and grow the canvas to fit
 * every frame, as browsers do, instead of cropping. */
static void frame_extents(gif_t *g, size_t start, int *w, int *h)
{
    gif_t *t = g;                                        /* reuse its reader; pos restored by caller */
    t->pos = start;
    for (int guard = 0; guard < 100000; guard++) {
        int b = rd8(t);
        if (b == 0x21) {
            if (rd8(t) < 0 || !skip_sub_blocks(t)) {
                return;
            }
        } else if (b == 0x2C) {
            if (!need(t, 9)) {
                return;
            }
            int fx = rd16(t), fy = rd16(t), fw = rd16(t), fh = rd16(t);
            uint8_t p = (uint8_t)rd8(t);
            if (p & 0x80) {
                size_t n = (size_t)3 * (2 << (p & 7));
                if (!need(t, n)) {
                    return;
                }
                t->pos += n;
            }
            if (rd8(t) < 0 || !skip_sub_blocks(t)) {    /* LZW min size + data */
                return;
            }
            if (fx + fw <= MAX_SIDE && fy + fh <= MAX_SIDE) {
                if (fx + fw > *w) *w = fx + fw;
                if (fy + fh > *h) *h = fy + fh;
            }
        } else {
            return;                                      /* trailer, end, or junk */
        }
    }
}

static void fill_rect(gif_t *g, int x, int y, int w, int h, uint16_t c)
{
    for (int j = y; j < y + h && j < g->h; j++) {
        for (int i = x; i < x + w && i < g->w; i++) {
            g->canvas[j * g->w + i] = c;
        }
    }
}

static void copy_rect(gif_t *g, uint16_t *dst, const uint16_t *src, int x, int y, int w, int h)
{
    for (int j = y; j < y + h && j < g->h; j++) {
        int n = (x + w > g->w ? g->w : x + w) - x;
        if (n > 0) {
            memcpy(dst + j * g->w + x, src + j * g->w + x, n * sizeof(uint16_t));
        }
    }
}

/* ------------------------------------------------------------------ API */
gif_t *gif_open(const uint8_t *data, size_t size)
{
    if (size < 13 || memcmp(data, "GIF8", 4) != 0) {
        return NULL;
    }
    gif_t *g = heap_caps_calloc(1, sizeof(gif_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g) {
        return NULL;
    }
    g->data = data;
    g->size = size;
    int w = data[6] | (data[7] << 8), h = data[8] | (data[9] << 8);
    uint8_t packed0 = data[10];
    size_t start = 13 + ((packed0 & 0x80) ? (size_t)3 * (2 << (packed0 & 7)) : 0);
    if (start <= size) {
        frame_extents(g, start, &w, &h);
    }
    if (w <= 0 || h <= 0 || w > MAX_SIDE || h > MAX_SIDE || (unsigned)(w * h) > MAX_PIXELS) {
        gif_close(g);
        return NULL;
    }
    g->w = w;
    g->h = h;
    g->canvas = heap_caps_calloc((size_t)w * h, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g->canvas) {
        gif_close(g);
        return NULL;
    }
    uint8_t packed = data[10];
    g->pos = 13;
    if (packed & 0x80) {
        if (!read_palette(g, g->gpal, 2 << (packed & 7))) {
            gif_close(g);
            return NULL;
        }
        g->has_gpal = true;
    }
    g->frames_start = g->pos;
    g->transparent = -1;
    return g;
}

int gif_width(const gif_t *g)  { return g->w; }
int gif_height(const gif_t *g) { return g->h; }
const uint16_t *gif_canvas(const gif_t *g) { return g->canvas; }

void gif_dirty(const gif_t *g, int *x, int *y, int *w, int *h)
{
    *x = g->dx0;
    *y = g->dy0;
    *w = g->dx1 - g->dx0;
    *h = g->dy1 - g->dy0;
}

int gif_next_frame(gif_t *g, int *delay_ms)
{
    if (g->is_still) {
        return 0;
    }
    bool looped = false;
    g->dx0 = g->dy0 = g->dx1 = g->dy1 = 0;
    for (;;) {
        int b = rd8(g);
        if (b == 0x21) {                                 /* extension */
            int label = rd8(g);
            if (label == 0xF9) {                         /* graphic control */
                int len = rd8(g);
                if (len < 4 || !need(g, len)) {
                    return -1;
                }
                uint8_t p = g->data[g->pos];
                g->disposal = (p >> 2) & 7;
                g->delay_cs = g->data[g->pos + 1] | (g->data[g->pos + 2] << 8);
                g->transparent = (p & 1) ? g->data[g->pos + 3] : -1;
                g->pos += len;
                if (!skip_sub_blocks(g)) {
                    return -1;
                }
            } else if (label < 0 || !skip_sub_blocks(g)) {
                return -1;
            }
        } else if (b == 0x2C) {                          /* image */
            if (!need(g, 9)) {
                return -1;
            }
            int fx = rd16(g), fy = rd16(g), fw = rd16(g), fh = rd16(g);
            uint8_t p = (uint8_t)rd8(g);
            static uint16_t lpal[256];                   /* one decoder at a time */
            const uint16_t *pal = g->gpal;
            if (p & 0x80) {
                if (!read_palette(g, lpal, 2 << (p & 7))) {
                    return -1;
                }
                pal = lpal;
            } else if (!g->has_gpal) {
                pal = lpal;                              /* no palette at all: black */
                memset(lpal, 0, sizeof(lpal));
            }

            /* undo the previous frame as it asked */
            if (g->prev_disposal == 2) {
                fill_rect(g, g->px, g->py, g->pw, g->ph, 0);
                dirty_add(g, g->px, g->py, g->pw, g->ph);
            } else if (g->prev_disposal == 3 && g->backup) {
                copy_rect(g, g->canvas, g->backup, g->px, g->py, g->pw, g->ph);
                dirty_add(g, g->px, g->py, g->pw, g->ph);
            }
            if (g->disposal == 3) {
                if (!g->backup) {
                    g->backup = heap_caps_malloc((size_t)g->w * g->h * sizeof(uint16_t),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                }
                if (g->backup) {
                    copy_rect(g, g->backup, g->canvas, fx, fy, fw, fh);
                }
            }
            if (fw <= 0 || fh <= 0 || !decode_image(g, fx, fy, fw, fh, p & 0x40, pal)) {
                return -1;
            }
            dirty_add(g, fx, fy, fw, fh);
            g->prev_disposal = g->disposal;
            g->px = fx;
            g->py = fy;
            g->pw = fw;
            g->ph = fh;
            g->frames_in_pass++;
            int d = g->delay_cs * 10;
            *delay_ms = d < 20 ? 100 : d;                /* like browsers: 0/1 cs means "fast" */
            /* the GCE applies to one image only */
            g->delay_cs = 0;
            g->transparent = -1;
            g->disposal = 0;
            return 1;
        } else if (b == 0x3B || b < 0) {                 /* trailer / end of data: loop */
            if (g->frames_in_pass == 0 || looped) {
                return -1;                               /* no image at all */
            }
            if (g->frames_in_pass == 1) {
                g->is_still = true;                      /* a still GIF: keep it as is */
                return 0;
            }
            looped = true;
            g->pos = g->frames_start;
            g->frames_in_pass = 0;
            g->prev_disposal = 0;
            memset(g->canvas, 0, (size_t)g->w * g->h * sizeof(uint16_t));
            dirty_add(g, 0, 0, g->w, g->h);              /* cleared: all of it changed */
        } else {
            return -1;
        }
    }
}

void gif_close(gif_t *g)
{
    if (!g) {
        return;
    }
    heap_caps_free(g->canvas);
    heap_caps_free(g->backup);
    heap_caps_free(g);
}
