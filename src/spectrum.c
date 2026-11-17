/*
 * Spectrum analyzer (see spectrum.h).
 *
 * Audio side: the player task writes a mono mix into a ring buffer (no lock;
 * a torn read only costs one slightly odd frame on screen).
 * UI side: an LVGL timer takes the newest 4096 samples, applies a Hann
 * window and an in-place radix-2 FFT, maps bins to bands, smooths (fast
 * attack, slower fall, peak hold) and redraws. 4096 points (11.7 Hz bins at
 * 48 kHz) keep a bass note in one or two bars; 1024 smeared it over ~8.
 * Nothing big lives on the stack: this runs in the 8 KB Arduino loopTask.
 *
 * Tapping the widget toggles a waterfall view: each frame becomes one pixel
 * row (one log-spaced column per pixel) pushed in at the top of an RGB565
 * image in PSRAM. The image is a ring of H rows stored twice (2H rows), so
 * the newest H rows are always contiguous and drawing needs no memmove.
 */
#include "spectrum.h"
#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "misc/cache/instance/lv_image_cache.h"

#define N           4096                 /* FFT size: ~85 ms at 48 kHz */
#define LOG2N       12
#define RING        8192                 /* power of two, >= N */
#define BANDS       36
#define GAP         5                    /* px between bars */
#define F_LO        40.0f
#define F_HI        16000.0f
#define DB_FLOOR    (-62.0f)             /* bottom of the display */
#define TILT_DB     3.0f                 /* per octave above/below 1 kHz */
#define FPS_MS      33

#define C_BAR_BOT   lv_color_hex(0x2F6BEA)
#define C_BAR_TOP   lv_color_hex(0xBFD6FF)
#define C_PEAK      lv_color_hex(0xEDEFF4)

/* ---- shared with the player task ---- */
static int16_t s_ring[RING];
static volatile uint32_t s_widx;
static volatile uint32_t s_rate = 48000;
static volatile int64_t s_last_feed_us;
static volatile bool s_active;

/* ---- UI side ---- */
static lv_obj_t *s_obj;
static lv_timer_t *s_timer;
/* ~72 KB of FFT tables, allocated in spectrum_create() (PSRAM);
 * NULL means the analyzer stays blank */
static float *s_re, *s_im, *s_win, *s_cos, *s_sin;
static uint16_t *s_rev;
static uint16_t s_band_lo[BANDS], s_band_hi[BANDS];   /* bin range [lo, hi] */
static float s_band_tilt[BANDS];
static uint32_t s_bands_rate;
static float s_level[BANDS], s_peak[BANDS];
static uint8_t s_hold[BANDS];

/* ---- waterfall ---- */
static bool s_waterfall;
static uint16_t *s_wf_buf;                 /* RGB565, s_wf_w x (2 * s_wf_h) */
static uint16_t *s_col_lo, *s_col_hi;      /* bin range per pixel column */
static float *s_col_tilt;
static int s_wf_w, s_wf_h, s_wf_pos;       /* s_wf_pos: newest row */
static uint32_t s_cols_rate;
static uint16_t s_lut[256];                /* level -> colour */
static lv_image_dsc_t s_wf_img;

void spectrum_feed(const int16_t *st, int frames, uint32_t rate)
{
    if (!s_active) {
        return;
    }
    uint32_t w = s_widx;
    for (int i = 0; i < frames; i++) {
        s_ring[(w + i) & (RING - 1)] = (int16_t)(((int32_t)st[2 * i] + st[2 * i + 1]) / 2);
    }
    s_widx = w + frames;
    s_rate = rate;
    s_last_feed_us = esp_timer_get_time();
}

static void init_tables(void)
{
    for (int i = 0; i < N; i++) {
        s_win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (N - 1));
        uint16_t r = 0;
        for (int b = 0; b < LOG2N; b++) {
            r |= ((i >> b) & 1) << (LOG2N - 1 - b);
        }
        s_rev[i] = r;
    }
    for (int i = 0; i < N / 2; i++) {
        s_cos[i] = cosf(2.0f * (float)M_PI * i / N);
        s_sin[i] = -sinf(2.0f * (float)M_PI * i / N);
    }
}

static void setup_bands(uint32_t rate)
{
    float hi = F_HI < rate * 0.45f ? F_HI : rate * 0.45f;
    float ratio = hi / F_LO;
    for (int b = 0; b < BANDS; b++) {
        float f0 = F_LO * powf(ratio, (float)b / BANDS);
        float f1 = F_LO * powf(ratio, (float)(b + 1) / BANDS);
        int lo = (int)ceilf(f0 * N / rate);
        int h = (int)floorf(f1 * N / rate);
        if (h < lo) {                                 /* narrower than a bin: nearest one */
            lo = h = (int)lroundf(sqrtf(f0 * f1) * N / rate);
        }
        if (lo < 1) lo = 1;
        if (h > N / 2 - 1) h = N / 2 - 1;
        if (lo > h) lo = h;
        s_band_lo[b] = (uint16_t)lo;
        s_band_hi[b] = (uint16_t)h;
        s_band_tilt[b] = TILT_DB * log2f(sqrtf(f0 * f1) / 1000.0f);
    }
    s_bands_rate = rate;
}

static void fft(void)
{
    for (int i = 0; i < N; i++) {                     /* bit-reversal permutation */
        int j = s_rev[i];
        if (j > i) {
            float t = s_re[i]; s_re[i] = s_re[j]; s_re[j] = t;
            t = s_im[i]; s_im[i] = s_im[j]; s_im[j] = t;
        }
    }
    for (int len = 2; len <= N; len <<= 1) {
        int half = len >> 1, step = N / len;
        for (int i = 0; i < N; i += len) {
            for (int k = 0; k < half; k++) {
                float wr = s_cos[k * step], wi = s_sin[k * step];
                int a = i + k, b = a + half;
                float tr = s_re[b] * wr - s_im[b] * wi;
                float ti = s_re[b] * wi + s_im[b] * wr;
                s_re[b] = s_re[a] - tr;
                s_im[b] = s_im[a] - ti;
                s_re[a] += tr;
                s_im[a] += ti;
            }
        }
    }
}

/* Windowed FFT of the newest N samples into s_re/s_im. */
static void fft_frame(void)
{
    uint32_t end = s_widx;
    for (int i = 0; i < N; i++) {
        s_re[i] = s_ring[(end - N + i) & (RING - 1)] * s_win[i];
        s_im[i] = 0.0f;
    }
    fft();
}

/* Loudest bin in [lo, hi] as a 0..1 display level. */
static float bin_level(int lo, int hi, float tilt)
{
    const float full = 32767.0f * N / 4.0f;           /* full-scale sine through a Hann window */
    float m2 = 0.0f;
    for (int k = lo; k <= hi; k++) {
        float v = s_re[k] * s_re[k] + s_im[k] * s_im[k];
        if (v > m2) {
            m2 = v;
        }
    }
    float db = 10.0f * log10f(m2 / (full * full) + 1e-12f) + tilt;
    float v = (db - DB_FLOOR) / -DB_FLOOR;
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static void analyse(float *target)
{
    if (s_rate != s_bands_rate) {
        setup_bands(s_rate);
    }
    fft_frame();
    for (int b = 0; b < BANDS; b++) {
        target[b] = bin_level(s_band_lo[b], s_band_hi[b], s_band_tilt[b]);
    }
}

/* ---- waterfall ---- */

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* black -> deep blue -> blue -> cyan -> yellow -> red -> white */
static void init_lut(void)
{
    static const uint8_t stops[][4] = {
        {   0,   0,   0,   0 }, {  50,  10,  14,  60 }, { 100,  47, 107, 234 },
        { 150,  40, 210, 230 }, { 195, 250, 220,  60 }, { 230, 240,  60,  40 },
        { 255, 255, 245, 235 },
    };
    for (int i = 0; i < 256; i++) {
        int s = 0;
        while (stops[s + 1][0] < i) {
            s++;
        }
        const uint8_t *a = stops[s], *b = stops[s + 1];
        int t = (i - a[0]) * 256 / (b[0] - a[0] ? b[0] - a[0] : 1);
        s_lut[i] = rgb565((uint8_t)(a[1] + (b[1] - a[1]) * t / 256),
                          (uint8_t)(a[2] + (b[2] - a[2]) * t / 256),
                          (uint8_t)(a[3] + (b[3] - a[3]) * t / 256));
    }
}

static void setup_cols(uint32_t rate)
{
    float hi = F_HI < rate * 0.45f ? F_HI : rate * 0.45f;
    float ratio = hi / F_LO;
    for (int x = 0; x < s_wf_w; x++) {
        float f0 = F_LO * powf(ratio, (float)x / s_wf_w);
        float f1 = F_LO * powf(ratio, (float)(x + 1) / s_wf_w);
        int lo = (int)lroundf(f0 * N / rate);
        int h = (int)lroundf(f1 * N / rate);
        if (lo < 1) lo = 1;
        if (h > N / 2 - 1) h = N / 2 - 1;
        if (h < lo) h = lo;
        s_col_lo[x] = (uint16_t)lo;
        s_col_hi[x] = (uint16_t)h;
        s_col_tilt[x] = TILT_DB * log2f(sqrtf(f0 * f1) / 1000.0f);
    }
    s_cols_rate = rate;
}

static void wf_free(void)
{
    heap_caps_free(s_wf_buf);
    heap_caps_free(s_col_lo);
    heap_caps_free(s_col_hi);
    heap_caps_free(s_col_tilt);
    s_wf_buf = NULL;
    s_col_lo = s_col_hi = NULL;
    s_col_tilt = NULL;
    s_wf_w = s_wf_h = 0;
}

/* (Re)allocates the image to the widget's content size; false if it can't. */
static bool wf_fit(void)
{
    lv_area_t a;
    lv_obj_get_content_coords(s_obj, &a);
    int w = lv_area_get_width(&a), h = lv_area_get_height(&a);
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (w == s_wf_w && h == s_wf_h && s_wf_buf) {
        return true;
    }
    wf_free();
    s_wf_buf = heap_caps_calloc((size_t)w * 2 * h, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_col_lo = heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_8BIT);
    s_col_hi = heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_8BIT);
    s_col_tilt = heap_caps_malloc(w * sizeof(float), MALLOC_CAP_8BIT);
    if (!s_wf_buf || !s_col_lo || !s_col_hi || !s_col_tilt) {
        wf_free();
        return false;
    }
    s_wf_w = w;
    s_wf_h = h;
    s_wf_pos = 0;
    setup_cols(s_rate);
    return true;
}

static void wf_clear(void)
{
    if (s_wf_buf) {
        memset(s_wf_buf, 0, (size_t)s_wf_w * 2 * s_wf_h * sizeof(uint16_t));
    }
}

static void wf_tick(void)
{
    if (!wf_fit()) {
        return;
    }
    bool fresh = s_re && esp_timer_get_time() - s_last_feed_us < 150000;
    if (!fresh) {
        return;                                       /* paused/stopped: freeze the picture */
    }
    if (s_rate != s_cols_rate) {
        setup_cols(s_rate);
    }
    fft_frame();
    s_wf_pos = (s_wf_pos + s_wf_h - 1) % s_wf_h;      /* newest row goes on top */
    uint16_t *r0 = s_wf_buf + (size_t)s_wf_pos * s_wf_w;
    uint16_t *r1 = r0 + (size_t)s_wf_h * s_wf_w;      /* its twin in the second copy */
    for (int x = 0; x < s_wf_w; x++) {
        float v = bin_level(s_col_lo[x], s_col_hi[x], s_col_tilt[x]);
        r0[x] = r1[x] = s_lut[(int)(v * 255.0f)];
    }
    lv_image_cache_drop(&s_wf_img);                   /* data pointer moves every frame */
    lv_obj_invalidate(s_obj);
}

static void wf_draw(lv_layer_t *layer, const lv_area_t *a)
{
    if (!s_wf_buf || lv_area_get_width(a) != s_wf_w || lv_area_get_height(a) != s_wf_h) {
        return;                                       /* resized: next tick refits */
    }
    memset(&s_wf_img, 0, sizeof(s_wf_img));
    s_wf_img.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_wf_img.header.cf = LV_COLOR_FORMAT_RGB565;
    s_wf_img.header.w = s_wf_w;
    s_wf_img.header.h = s_wf_h;
    s_wf_img.header.stride = s_wf_w * sizeof(uint16_t);
    s_wf_img.data = (const uint8_t *)(s_wf_buf + (size_t)s_wf_pos * s_wf_w);
    s_wf_img.data_size = (uint32_t)s_wf_w * s_wf_h * sizeof(uint16_t);

    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = &s_wf_img;
    lv_draw_image(layer, &d, a);
}

static void tick(lv_timer_t *t)
{
    if (s_waterfall) {
        wf_tick();
        return;
    }
    float target[BANDS] = { 0 };
    bool fresh = s_re && esp_timer_get_time() - s_last_feed_us < 150000;   /* paused/stopped -> fall */
    if (fresh) {
        analyse(target);
    }
    bool moving = false;
    for (int b = 0; b < BANDS; b++) {
        float l = s_level[b];
        l = target[b] > l ? l + (target[b] - l) * 0.65f : l - (l - target[b]) * 0.18f - 0.004f;
        if (l < 0.0f) l = 0.0f;
        s_level[b] = l;
        if (l >= s_peak[b]) {
            s_peak[b] = l;
            s_hold[b] = 14;                                         /* ~0.45 s */
        } else if (s_hold[b]) {
            s_hold[b]--;
        } else {
            s_peak[b] -= 0.018f;
            if (s_peak[b] < l) s_peak[b] = l;
        }
        moving |= l > 0.0f || s_peak[b] > 0.0f;
    }
    if (moving || fresh) {
        lv_obj_invalidate(s_obj);
    }
}

static void draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_content_coords(s_obj, &a);
    if (s_waterfall) {
        wf_draw(layer, &a);
        return;
    }
    const int w = lv_area_get_width(&a), h = lv_area_get_height(&a) - 6;   /* room for peak caps */
    if (w <= 0 || h <= 0) {
        return;
    }
    const float bw = (w - (BANDS - 1) * GAP) / (float)BANDS;

    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.radius = 3;
    bar.bg_opa = LV_OPA_COVER;
    bar.bg_grad.dir = LV_GRAD_DIR_VER;
    bar.bg_grad.stops_count = 2;
    bar.bg_grad.stops[0].opa = LV_OPA_COVER;
    bar.bg_grad.stops[0].frac = 0;
    bar.bg_grad.stops[1].color = C_BAR_BOT;
    bar.bg_grad.stops[1].opa = LV_OPA_COVER;
    bar.bg_grad.stops[1].frac = 255;

    lv_draw_rect_dsc_t cap;
    lv_draw_rect_dsc_init(&cap);
    cap.radius = 1;
    cap.bg_opa = LV_OPA_80;
    cap.bg_color = C_PEAK;

    for (int b = 0; b < BANDS; b++) {
        int x1 = a.x1 + (int)lroundf(b * (bw + GAP));
        int x2 = a.x1 + (int)lroundf(b * (bw + GAP) + bw) - 1;
        int bh = (int)(s_level[b] * h);
        if (bh < 3) {
            bh = 3;                                    /* idle: a row of short dots */
        }
        lv_area_t r = { x1, a.y2 - bh + 1, x2, a.y2 };
        /* top colour follows the bar's height: taller bars glow brighter */
        bar.bg_grad.stops[0].color = lv_color_mix(C_BAR_TOP, C_BAR_BOT, (uint8_t)(s_level[b] * 255));
        lv_draw_rect(layer, &bar, &r);
        if (s_peak[b] > 0.02f) {
            int py = a.y2 - (int)(s_peak[b] * h) - 5;
            lv_area_t c = { x1, py, x2, py + 2 };
            lv_draw_rect(layer, &cap, &c);
        }
    }
}

void spectrum_set_waterfall(bool on)
{
    s_waterfall = on;
    memset(s_level, 0, sizeof(s_level));
    memset(s_peak, 0, sizeof(s_peak));
    wf_clear();
    if (s_obj) {
        lv_obj_invalidate(s_obj);
    }
}

bool spectrum_get_waterfall(void)
{
    return s_waterfall;
}

static void clicked_cb(lv_event_t *e)
{
    spectrum_set_waterfall(!s_waterfall);
    lv_obj_send_event(s_obj, LV_EVENT_VALUE_CHANGED, NULL);
}

/* PSRAM: internal RAM is scarce (the SD driver and the Wi-Fi link need it
 * for DMA), and the caches make PSRAM plenty fast for a 30 fps FFT. */
static void *alloc_fast(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

lv_obj_t *spectrum_create(lv_obj_t *parent)
{
    float *f = alloc_fast(sizeof(float) * (3 * N + N));      /* re, im, win, cos+sin */
    uint16_t *rev = alloc_fast(sizeof(uint16_t) * N);
    if (f && rev) {
        s_re = f;
        s_im = f + N;
        s_win = f + 2 * N;
        s_cos = f + 3 * N;
        s_sin = f + 3 * N + N / 2;
        s_rev = rev;
        init_tables();
        setup_bands(48000);
        init_lut();
    } else {
        heap_caps_free(f);
        heap_caps_free(rev);
    }
    s_obj = lv_obj_create(parent);
    lv_obj_remove_style_all(s_obj);
    lv_obj_add_flag(s_obj, LV_OBJ_FLAG_CLICKABLE);       /* tap: bars <-> waterfall */
    lv_obj_add_event_cb(s_obj, clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_remove_flag(s_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_obj, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(s_obj, LV_OBJ_FLAG_HIDDEN);
    s_timer = lv_timer_create(tick, FPS_MS, NULL);
    lv_timer_pause(s_timer);
    return s_obj;
}

void spectrum_set_active(bool on)
{
    s_active = on;
    if (on) {
        memset(s_level, 0, sizeof(s_level));
        memset(s_peak, 0, sizeof(s_peak));
        lv_obj_remove_flag(s_obj, LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(s_timer);
        wf_clear();
    } else {
        lv_timer_pause(s_timer);
        lv_obj_add_flag(s_obj, LV_OBJ_FLAG_HIDDEN);
    }
}
