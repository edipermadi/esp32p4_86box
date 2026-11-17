/*
 * Image-glyph font in the style of LVGL's lv_imgfont (see emoji_font.h).
 * LVGL draws with a single draw unit and LV_OS_NONE here, so the lazy cache
 * needs no locking.
 */
#include "emoji_font.h"
#include "emoji_data.h"
#include "font_ui.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "miniz.h"                     /* ROM tinfl */

static const char *TAG = "emoji";

lv_font_t emoji_font_16;
lv_font_t emoji_font_20;
lv_font_t emoji_font_24;

static lv_image_dsc_t *s_cache[EMOJI_COUNT];

static int find(uint32_t cp)
{
    int lo = 0, hi = EMOJI_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (emoji_cp[mid] == cp) {
            return mid;
        }
        if (emoji_cp[mid] < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

/* Parts of emoji sequences that should take no space on their own */
static bool is_invisible(uint32_t cp)
{
    return cp == 0x200D ||                       /* zero width joiner */
           cp == 0xFE0E || cp == 0xFE0F ||       /* text / emoji presentation selectors */
           cp == 0x20E3 ||                       /* combining keycap */
           (cp >= 0x1F3FB && cp <= 0x1F3FF) ||   /* skin tone modifiers */
           (cp >= 0xE0020 && cp <= 0xE007F);     /* tag characters (subdivision flags) */
}

static const lv_image_dsc_t *get_image(int i)
{
    if (s_cache[i]) {
        return s_cache[i];
    }
    const uint32_t w = emoji_w[i], h = EMOJI_HEIGHT;
    const size_t raw = (size_t)w * h * 3;
    lv_image_dsc_t *img = heap_caps_malloc(sizeof(*img) + raw, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!img) {
        img = malloc(sizeof(*img) + raw);
    }
    if (!img) {
        return NULL;
    }
    uint8_t *data = (uint8_t *)(img + 1);
    /* Not tinfl_decompress_mem_to_mem(): it keeps its ~11 KB decompressor
     * state on the stack, which overflows the 8 KB Arduino loopTask that LVGL
     * draws from. One static state instead (drawing is single-threaded). */
    static tinfl_decompressor s_inflate;
    tinfl_init(&s_inflate);
    size_t in_len = emoji_off[i + 1] - emoji_off[i];
    size_t n = raw;
    tinfl_status st = tinfl_decompress(&s_inflate, emoji_blob + emoji_off[i], &in_len,
                                       data, data, &n, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (st != TINFL_STATUS_DONE || n != raw) {
        ESP_LOGW(TAG, "U+%05lX: decompress failed", (unsigned long)emoji_cp[i]);
        free(img);
        return NULL;
    }
    memset(img, 0, sizeof(*img));
    img->header.magic = LV_IMAGE_HEADER_MAGIC;
    img->header.cf = LV_COLOR_FORMAT_RGB565A8;
    img->header.w = w;
    img->header.h = h;
    img->header.stride = w * 2;                  /* of the RGB565 plane */
    img->data_size = raw;
    img->data = data;
    s_cache[i] = img;
    return img;
}

static bool get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *g, uint32_t cp, uint32_t next)
{
    if (cp < 0x2000) {                           /* fast path: plain text */
        return false;
    }
    if (is_invisible(cp)) {
        g->format = LV_FONT_GLYPH_FORMAT_NONE;
        g->adv_w = g->box_w = g->box_h = 0;
        g->is_placeholder = 0;
        return true;
    }
    int i = find(cp);
    if (i < 0) {
        return false;                            /* -> fallback text font */
    }
    const int32_t lh = font->line_height, bl = font->base_line, h = EMOJI_HEIGHT;
    g->format = LV_FONT_GLYPH_FORMAT_IMAGE;
    g->is_placeholder = 0;
    g->box_w = emoji_w[i];
    g->box_h = h;
    g->adv_w = emoji_w[i] + 2;                   /* a little air between emoji */
    g->ofs_x = 1;
    g->ofs_y = (int16_t)((lh - bl) - h - (lh - h) / 2);   /* centred in the line */
    g->gid.index = (uint32_t)i;
    return true;
}

static const void *get_glyph_bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *draw_buf)
{
    (void)draw_buf;
    return get_image((int)g->gid.index);
}

static void setup(lv_font_t *f, const lv_font_t *text)
{
    memset(f, 0, sizeof(*f));
    f->get_glyph_dsc = get_glyph_dsc;
    f->get_glyph_bitmap = get_glyph_bitmap;
    f->fallback = text;
    f->line_height = text->line_height;
    f->base_line = text->base_line;
    f->cap_height = text->cap_height;
    f->x_height = text->x_height;
    f->subpx = LV_FONT_SUBPX_NONE;
    f->kerning = text->kerning;                  /* keeps kerning on for the text font */
    f->underline_position = text->underline_position;
    f->underline_thickness = text->underline_thickness;
}

void emoji_fonts_init(void)
{
    setup(&emoji_font_16, &font_ui_16);
    setup(&emoji_font_20, &font_ui_20);
    setup(&emoji_font_24, &font_ui_24);
}
