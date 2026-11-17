/*
 * Gallery / slideshow page (see gallery.h).
 *
 * Two screen-sized RGB565 buffers in PSRAM: the front one is on screen, the
 * back one receives the next slide. A worker task scans the folder and loads
 * files (reading under bsp_sdcard_fs_lock so a player rescan can't remount
 * mid-read); for a JPEG it also decodes (hardware) and scales into the back
 * buffer. The LVGL side swaps buffers when a slide is ready, plays GIF frames
 * (in place in the front buffer, redrawing only the changed area) and runs
 * the 30 s clock. Slides are always full-screen: fitted, centred, black
 * borders.
 */
#include "gallery.h"
#include "gif_dec.h"
#include "img_scale.h"
#include "jpeg_sw.h"
#include "settings.h"
#include "ui_theme.h"
#include "fonts/font_ui.h"
#include "fonts/emoji_font.h"
#include "bsp/bsp_board.h"
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_err.h"
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "misc/cache/instance/lv_image_cache.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define SLIDE_MS        30000
#define BAR_HIDE_MS     4000
#define MAX_FILES       2000
#define MAX_DEPTH       3
#define MAX_FILE_BYTES  (24 * 1024 * 1024)
#define GIVE_UP_AFTER   25                  /* consecutive failures before stopping */

/* ------------------------------------------------------- worker protocol */
typedef enum { REQ_SCAN, REQ_LOAD } req_kind_t;
typedef struct {
    req_kind_t kind;
    int pos;                        /* REQ_LOAD: position in the play order */
    uint16_t *dst;                  /* REQ_LOAD: back buffer for a JPEG */
    char path[300];
} req_t;

typedef enum { RES_LIST, RES_JPEG, RES_GIF, RES_FAIL } res_kind_t;
typedef struct {
    res_kind_t kind;
    int pos;
    gif_t *gif;                     /* RES_GIF (+ its file data) */
    uint8_t *gif_data;
    char **files;                   /* RES_LIST */
    int nfiles;
    char why[96];                   /* RES_FAIL: what went wrong, with details */
} res_t;

static QueueHandle_t s_req, s_res;
static int s_dw, s_dh;              /* screen size */
static uint16_t *s_buf[2];
static jpeg_decoder_handle_t s_jpeg;

/* ---------------------------------------------------------- LVGL state */
static lv_obj_t *s_scr, *s_img, *s_msg, *s_bar, *s_name, *s_count;
static lv_image_dsc_t s_dsc[2];
static int s_front;                 /* index of the buffer on screen */
static lv_timer_t *s_timer;
static bool s_shown;

static char **s_files;
static int s_n;
static int *s_order;
static int s_pos = -1;              /* position in s_order on screen */
static char s_cur[300];             /* its path, to find it again after a rescan */
static bool s_busy;                 /* a request is out to the worker */
static int s_load_buf;              /* back buffer the pending JPEG goes into */
static bool s_scan_pending;         /* rescan asked for while a load was out */
static int s_dir = 1;               /* +1 forward, -1 back (for skipping bad files) */
static int s_fail_streak;
static int64_t s_slide_since, s_bar_since, s_next_frame;
static bool s_gestured;

static gif_t *s_gif;
static uint8_t *s_gif_data;
static int s_gif_w, s_gif_h;        /* GIF scaled size on screen */
static int s_gif_ox, s_gif_oy;      /* ...and its offset (centred, black borders) */

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static bool buffers_ok(void) { return s_buf[0] && s_buf[1]; }

/* ================================================================ worker */
static bool has_ext(const char *n, const char *ext)
{
    size_t a = strlen(n), b = strlen(ext);
    return a > b && strcasecmp(n + a - b, ext) == 0;
}

/* Everything the gallery keeps (file list, play order) lives in PSRAM: with
 * ~2000 files, small mallocs would land in internal RAM and starve the SD
 * driver, which needs internal DMA memory for its sector bounce buffers. */
#define PS_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static char *ps_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = heap_caps_malloc(n, PS_CAPS);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

static bool is_jpeg(const char *n) { return has_ext(n, ".jpg") || has_ext(n, ".jpeg"); }
static bool is_gif(const char *n)  { return has_ext(n, ".gif"); }

static void scan_dir(const char *dir, int depth, char ***list, int *n, int *cap)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && *n < MAX_FILES) {
        if (e->d_name[0] == '.') {
            continue;                               /* hidden, macOS ._ files */
        }
        char path[300];
        if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path)) {
            continue;
        }
        if (e->d_type == DT_DIR) {
            if (depth < MAX_DEPTH) {
                scan_dir(path, depth + 1, list, n, cap);
            }
        } else if (is_jpeg(e->d_name) || is_gif(e->d_name)) {
            if (*n == *cap) {
                int nc = *cap ? *cap * 2 : 64;
                char **nl = heap_caps_realloc(*list, nc * sizeof(char *), PS_CAPS);
                if (!nl) {
                    break;
                }
                *list = nl;
                *cap = nc;
            }
            char *s = ps_strdup(path);
            if (!s) {
                break;
            }
            (*list)[(*n)++] = s;
        }
    }
    closedir(d);
}

static int cmp_path(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

/* Reads a whole file into buf (allocated by alloc(size)). */
static uint8_t *read_file(const char *path, size_t *size, bool jpeg_input, char *why, size_t wn)
{
    uint8_t *buf = NULL;
    bsp_sdcard_fs_lock();
    struct stat st;
    FILE *f = NULL;
    if (stat(path, &st) != 0) {
        snprintf(why, wn, "stat failed (errno %d)", errno);
    } else if (st.st_size <= 0) {
        snprintf(why, wn, "Empty file");
    } else if (st.st_size > MAX_FILE_BYTES) {
        snprintf(why, wn, "File too big (%ld MB)", (long)(st.st_size >> 20));
    } else if (!(f = fopen(path, "rb"))) {
        snprintf(why, wn, "fopen failed (errno %d)", errno);
    } else {
        size_t got = 0;
        if (jpeg_input) {
            jpeg_decode_memory_alloc_cfg_t mc = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
            buf = jpeg_alloc_decoder_mem(st.st_size, &mc, &got);
        } else {
            buf = heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        size_t rd = 0;
        if (!buf) {
            snprintf(why, wn, "No memory for %ld KB file (PSRAM largest free %u KB)",
                     (long)(st.st_size >> 10),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >> 10));
        } else if ((rd = fread(buf, 1, st.st_size, f)) != (size_t)st.st_size) {
            snprintf(why, wn, "Read error: %u of %ld bytes (errno %d)", (unsigned)rd,
                     (long)st.st_size, errno);
            free(buf);
            buf = NULL;
        } else {
            *size = st.st_size;
        }
    }
    if (f) {
        fclose(f);
    }
    bsp_sdcard_fs_unlock();
    return buf;
}

/* Every slide fills the whole screen buffer: picture fitted (aspect kept)
 * and centred, the rest black. The image widget never changes size or
 * position, so nothing moves between slides. */
static bool letterbox(const uint16_t *src, int sw, int sh, int sstride, uint16_t *dst)
{
    int w, h;
    img_fit(sw, sh, s_dw, s_dh, &w, &h);
    memset(dst, 0, (size_t)s_dw * s_dh * sizeof(uint16_t));
    uint16_t *at = dst + (size_t)((s_dh - h) / 2) * s_dw + (s_dw - w) / 2;
    return img_scale_ex(src, sw, sh, sstride, at, s_dw, w, h, 0, 0, w, h);
}

/* Hardware decode + scale into rq->dst. Returns 1 done, 0 "hardware can't do
 * this file" (try software), -1 failed for good (reason in rs->why). */
static int jpeg_hw(const req_t *rq, res_t *rs, const uint8_t *in, size_t size)
{
    jpeg_decode_picture_info_t info;
    esp_err_t err = jpeg_decoder_get_info(in, size, &info);
    if (err != ESP_OK || !info.width || !info.height) {
        return 0;                                    /* no SOF0: progressive, or exotic */
    }
    if ((info.width * info.height) % 8) {
        return 0;                                    /* a hardware limitation */
    }
    if (!s_jpeg) {
        jpeg_decode_engine_cfg_t ec = { .intr_priority = 0, .timeout_ms = 5000 };
        err = jpeg_new_decoder_engine(&ec, &s_jpeg);
        if (err != ESP_OK) {
            s_jpeg = NULL;
            return 0;
        }
    }
    /* the hardware pads rows/columns to whole MCUs (8 or 16 px) */
    int mx = 8, my = 8;
    if (info.sample_method == JPEG_DOWN_SAMPLING_YUV420) {
        mx = my = 16;
    } else if (info.sample_method == JPEG_DOWN_SAMPLING_YUV422) {
        mx = 16;
    }
    int pw = (info.width + mx - 1) / mx * mx, ph = (info.height + my - 1) / my * my;
    size_t need = (size_t)pw * ph * 2, got = 0;
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (need + 64 * 1024 > largest) {
        snprintf(rs->why, sizeof(rs->why), "Too large: %ux%u needs %u KB, PSRAM block %u KB",
                 (unsigned)info.width, (unsigned)info.height, (unsigned)(need >> 10),
                 (unsigned)(largest >> 10));
        return -1;                                   /* software would need even more */
    }
    jpeg_decode_memory_alloc_cfg_t mc = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
    uint8_t *out = jpeg_alloc_decoder_mem(need, &mc, &got);
    if (!out) {
        snprintf(rs->why, sizeof(rs->why), "No memory for the %ux%u decode buffer",
                 (unsigned)info.width, (unsigned)info.height);
        return -1;
    }
    jpeg_decode_cfg_t dc = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,      /* little endian, as LVGL wants */
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    err = jpeg_decoder_process(s_jpeg, &dc, in, size, out, got, &out_size);
    if (err != ESP_OK) {
        free(out);
        return 0;                                    /* let software have a go */
    }
    bool ok = letterbox((const uint16_t *)out, info.width, info.height, pw, rq->dst);
    free(out);
    if (!ok) {
        snprintf(rs->why, sizeof(rs->why), "No memory to scale");
        return -1;
    }
    return 1;
}

static bool load_jpeg(const req_t *rq, res_t *rs)
{
    size_t size = 0;
    uint8_t *in = read_file(rq->path, &size, true, rs->why, sizeof(rs->why));
    if (!in) {
        return false;
    }
    int r = jpeg_hw(rq, rs, in, size);
    if (r == 0) {
        /* progressive (or anything else the hardware refuses): stb_image */
        int w = 0, h = 0;
        uint16_t *px = jpeg_sw_decode(in, size, &w, &h, rs->why, sizeof(rs->why));
        free(in);
        in = NULL;
        if (px) {
            r = letterbox(px, w, h, w, rq->dst) ? 1 : -1;
            if (r < 0) {
                snprintf(rs->why, sizeof(rs->why), "No memory to scale");
            }
            jpeg_sw_free(px);
        } else {
            r = -1;
        }
    }
    free(in);
    return r == 1;
}

static bool load_gif(const req_t *rq, res_t *rs)
{
    size_t size = 0;
    uint8_t *data = read_file(rq->path, &size, false, rs->why, sizeof(rs->why));
    if (!data) {
        return false;
    }
    gif_t *g = gif_open(data, size);
    if (!g) {
        if (size >= 10 && memcmp(data, "GIF8", 4) == 0) {
            snprintf(rs->why, sizeof(rs->why), "GIF too large or no memory (%ux%u; max 4096 a side, 4 Mpx)",
                     data[6] | (data[7] << 8), data[8] | (data[9] << 8));
        } else {
            snprintf(rs->why, sizeof(rs->why), "Not a GIF (starts %02X %02X %02X %02X)",
                     size > 0 ? data[0] : 0, size > 1 ? data[1] : 0, size > 2 ? data[2] : 0,
                     size > 3 ? data[3] : 0);
        }
        heap_caps_free(data);
        data = NULL;
        return false;
    }
    rs->gif = g;
    rs->gif_data = data;
    return true;
}

static void worker(void *arg)
{
    req_t rq;
    for (;;) {
        if (xQueueReceive(s_req, &rq, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        res_t rs = { 0 };
        rs.pos = rq.pos;
        if (rq.kind == REQ_SCAN) {
            char **list = NULL;
            int n = 0, cap = 0;
            bsp_sdcard_fs_lock();
            scan_dir(BSP_SD_MOUNT_POINT "/" GALLERY_DIR, 0, &list, &n, &cap);
            bsp_sdcard_fs_unlock();
            if (n > 1) {
                qsort(list, n, sizeof(char *), cmp_path);
            }
            rs.kind = RES_LIST;
            rs.files = list;
            rs.nfiles = n;
        } else if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) < 8 * 1024) {
            /* the SD driver needs a little internal DMA RAM for each read */
            rs.kind = RES_FAIL;
            snprintf(rs.why, sizeof(rs.why), "Internal RAM low (%u KB free)",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
        } else if (is_gif(rq.path)) {
            rs.kind = load_gif(&rq, &rs) ? RES_GIF : RES_FAIL;
        } else {
            rs.kind = load_jpeg(&rq, &rs) ? RES_JPEG : RES_FAIL;
        }
        xQueueSend(s_res, &rs, portMAX_DELAY);
    }
}

/* ============================================================ LVGL side */
static void free_list(char **l, int n)
{
    for (int i = 0; i < n; i++) {
        heap_caps_free(l[i]);
    }
    heap_caps_free(l);
}

static void close_gif(void)
{
    gif_close(s_gif);
    heap_caps_free(s_gif_data);
    s_gif = NULL;
    s_gif_data = NULL;
}

static void build_order(void)
{
    heap_caps_free(s_order);
    s_order = s_n ? heap_caps_malloc(s_n * sizeof(int), PS_CAPS) : NULL;
    if (!s_order) {
        s_n = 0;
        return;
    }
    for (int i = 0; i < s_n; i++) {
        s_order[i] = i;
    }
    if (settings_get()->gallery_random) {
        for (int i = s_n - 1; i > 0; i--) {                 /* Fisher-Yates */
            int j = esp_random() % (i + 1);
            int t = s_order[i];
            s_order[i] = s_order[j];
            s_order[j] = t;
        }
    }
}

static void set_msg(const char *txt)
{
    if (txt) {
        lv_label_set_text(s_msg, txt);
        lv_obj_remove_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_bar_text(void)
{
    if (s_pos < 0 || s_pos >= s_n) {
        lv_label_set_text(s_name, "Gallery");
        lv_label_set_text(s_count, "");
        return;
    }
    const char *p = s_files[s_order[s_pos]];
    const char *base = strrchr(p, '/');
    lv_label_set_text(s_name, base ? base + 1 : p);
    lv_label_set_text_fmt(s_count, "%d / %d", s_pos + 1, s_n);
}

static void request_load(int pos)
{
    if (s_n == 0 || s_busy) {
        return;
    }
    pos = ((pos % s_n) + s_n) % s_n;
    s_load_buf = !s_front;              /* GIF frames don't swap while s_busy (see tick) */
    req_t rq = { .kind = REQ_LOAD, .pos = pos, .dst = s_buf[s_load_buf] };
    strlcpy(rq.path, s_files[s_order[pos]], sizeof(rq.path));
    s_busy = true;
    xQueueSend(s_req, &rq, portMAX_DELAY);
}

static void request_scan(void)
{
    if (s_busy) {
        s_scan_pending = true;                       /* sent when the load is in */
        return;
    }
    s_scan_pending = false;
    req_t rq = { .kind = REQ_SCAN };
    s_busy = true;
    xQueueSend(s_req, &rq, portMAX_DELAY);
}

/* Next slide in the current direction; reshuffles after a full random round. */
static void advance(int dir)
{
    if (s_n == 0) {
        return;
    }
    s_dir = dir;
    int next = s_pos + dir;
    if (next >= s_n && settings_get()->gallery_random && s_n > 2) {
        int last = s_order[s_n - 1];
        build_order();
        if (s_order[0] == last) {                    /* no immediate repeat */
            s_order[0] = s_order[s_n - 1];
            s_order[s_n - 1] = last;
        }
        next = 0;
    }
    request_load(next);
}

/* Puts (full-screen) buffer b on screen. */
static void present(int b)
{
    lv_image_dsc_t *d = &s_dsc[b];
    lv_image_cache_drop(d);
    memset(d, 0, sizeof(*d));
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = s_dw;
    d->header.h = s_dh;
    d->header.stride = s_dw * 2;
    d->data = (const uint8_t *)s_buf[b];
    d->data_size = (uint32_t)s_dw * s_dh * 2;
    s_front = b;
    lv_image_set_src(s_img, d);
    set_msg(NULL);
}

/* Next GIF frame. The first one is drawn into the back buffer and swapped in;
 * after that frames update the front buffer in place, redrawing (and
 * invalidating) only the part that changed - no full-screen repaint per
 * frame, so no flicker. LVGL renders in this same task, so it never reads
 * the buffer while it is being written. */
static bool gif_show_frame(bool first)
{
    int delay = 100;
    int r = gif_next_frame(s_gif, &delay);
    if (r < 0) {
        return false;
    }
    const int gw = gif_width(s_gif), gh = gif_height(s_gif);
    if (first) {
        int b = !s_front;
        memset(s_buf[b], 0, (size_t)s_dw * s_dh * sizeof(uint16_t));
        uint16_t *at = s_buf[b] + (size_t)s_gif_oy * s_dw + s_gif_ox;
        if (!img_scale_ex(gif_canvas(s_gif), gw, gh, gw, at, s_dw, s_gif_w, s_gif_h,
                          0, 0, s_gif_w, s_gif_h)) {
            return false;
        }
        present(b);
    } else if (r == 1) {
        int x, y, w, h;
        gif_dirty(s_gif, &x, &y, &w, &h);
        if (w > 0 && h > 0) {
            /* canvas rect -> screen rect, one pixel of slack for the filter */
            int x0 = (int)((int64_t)x * s_gif_w / gw) - 1;
            int y0 = (int)((int64_t)y * s_gif_h / gh) - 1;
            int x1 = (int)(((int64_t)(x + w) * s_gif_w + gw - 1) / gw) + 1;
            int y1 = (int)(((int64_t)(y + h) * s_gif_h + gh - 1) / gh) + 1;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > s_gif_w) x1 = s_gif_w;
            if (y1 > s_gif_h) y1 = s_gif_h;
            uint16_t *at = s_buf[s_front] + (size_t)s_gif_oy * s_dw + s_gif_ox;
            if (!img_scale_ex(gif_canvas(s_gif), gw, gh, gw, at, s_dw, s_gif_w, s_gif_h,
                              x0, y0, x1, y1)) {
                return false;
            }
            lv_area_t a;
            lv_obj_get_coords(s_img, &a);
            a.x1 += s_gif_ox + x0;
            a.y1 += s_gif_oy + y0;
            a.x2 = a.x1 + (x1 - x0) - 1;
            a.y2 = a.y1 + (y1 - y0) - 1;
            lv_obj_invalidate_area(s_img, &a);
        }
    }
    if (r != 1) {
        s_next_frame = INT64_MAX;                    /* still GIF: no more frames */
    } else if (first || now_ms() - s_next_frame > 250) {
        s_next_frame = now_ms() + delay;             /* start, or far behind: no burst */
    } else {
        s_next_frame += delay;                       /* from the due time, so decode and
                                                      * draw time don't stretch each frame */
    }
    return true;
}

/* Why the pictures of the current failure streak failed (reset on success). */
#define N_REASONS 6
static struct { char why[96]; int n; char example[64]; } s_reasons[N_REASONS];
static bool s_gave_up;

static void reasons_reset(void)
{
    memset(s_reasons, 0, sizeof(s_reasons));
}

static void reasons_add(const char *why, const char *path)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    printf("gallery: %s: %s\n", path, why);         /* Serial Monitor, 115200 */
    int free_slot = -1;
    for (int i = 0; i < N_REASONS; i++) {
        if (s_reasons[i].n && strcmp(s_reasons[i].why, why) == 0) {
            s_reasons[i].n++;
            return;
        }
        if (!s_reasons[i].n && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot >= 0) {
        strlcpy(s_reasons[free_slot].why, why, sizeof(s_reasons[0].why));
        strlcpy(s_reasons[free_slot].example, base, sizeof(s_reasons[0].example));
        s_reasons[free_slot].n = 1;
    }
}

static void on_fail(const char *why)
{
    if (s_pos >= 0 && s_pos < s_n) {
        reasons_add(why, s_files[s_order[s_pos]]);
    }
    s_fail_streak++;
    if (s_fail_streak >= s_n || s_fail_streak >= GIVE_UP_AFTER) {
        /* stop and say why, most common reason first */
        close_gif();
        s_gave_up = true;
        char msg[600];
        int len = snprintf(msg, sizeof(msg), "Could not show the last %d picture%s.\n\n",
                           s_fail_streak, s_fail_streak == 1 ? "" : "s");
        bool used[N_REASONS] = { 0 };
        for (int k = 0; k < 3; k++) {
            int best = -1;
            for (int i = 0; i < N_REASONS; i++) {
                if (s_reasons[i].n && !used[i] && (best < 0 || s_reasons[i].n > s_reasons[best].n)) {
                    best = i;
                }
            }
            if (best < 0 || len >= (int)sizeof(msg)) {
                break;
            }
            used[best] = true;
            len += snprintf(msg + len, sizeof(msg) - len, "%dx  %s\n    e.g. %s\n",
                            s_reasons[best].n, s_reasons[best].why, s_reasons[best].example);
        }
        if (len < (int)sizeof(msg)) {
            snprintf(msg + len, sizeof(msg) - len, "\nGo back and open the gallery again to retry.");
        }
        set_msg(msg);
        lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_LEFT, 0);
        return;
    }
    s_pos = s_pos < 0 ? 0 : s_pos;
    advance(s_dir);
}

static void handle_result(const res_t *rs)
{
    s_busy = false;
    switch (rs->kind) {
    case RES_LIST: {
        free_list(s_files, s_n);
        s_files = rs->files;
        s_n = rs->nfiles;
        build_order();
        s_fail_streak = 0;
        s_gave_up = false;
        reasons_reset();
        lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
        if (s_n == 0) {
            close_gif();
            s_pos = -1;
            s_cur[0] = 0;
            set_msg("No pictures yet.\n\nPut .jpg and .gif files in the \"" GALLERY_DIR
                    "\" folder on the SD card.");
            show_bar_text();
            return;
        }
        int start = 0;                               /* carry on where we were */
        for (int i = 0; i < s_n && s_cur[0]; i++) {
            if (strcmp(s_files[s_order[i]], s_cur) == 0) {
                start = i;
                break;
            }
        }
        s_pos = start;
        request_load(start);
        return;
    }
    case RES_JPEG:
        close_gif();
        s_fail_streak = 0;
        reasons_reset();
        s_pos = rs->pos;
        present(s_load_buf);
        break;
    case RES_GIF:
        close_gif();
        s_gif = rs->gif;
        s_gif_data = rs->gif_data;
        img_fit(gif_width(s_gif), gif_height(s_gif), s_dw, s_dh, &s_gif_w, &s_gif_h);
        s_gif_ox = (s_dw - s_gif_w) / 2;
        s_gif_oy = (s_dh - s_gif_h) / 2;
        s_pos = rs->pos;
        if (!gif_show_frame(true)) {
            close_gif();
            on_fail("GIF data is broken");
            return;
        }
        s_fail_streak = 0;
        reasons_reset();
        break;
    case RES_FAIL:
        s_pos = rs->pos;                             /* so skipping moves past it */
        on_fail(rs->why[0] ? rs->why : "Unknown error");
        return;
    }
    strlcpy(s_cur, s_files[s_order[s_pos]], sizeof(s_cur));
    s_slide_since = now_ms();
    show_bar_text();
}

static void tick(lv_timer_t *t)
{
    res_t rs;
    while (xQueueReceive(s_res, &rs, 0) == pdTRUE) {
        handle_result(&rs);
    }
    if (s_scan_pending && !s_busy) {
        request_scan();
    }
    int64_t now = now_ms();
    /* frames update the front buffer; a load in flight only touches the back one */
    if (s_gif && now >= s_next_frame && !gif_show_frame(false)) {
        close_gif();
        on_fail("GIF data is broken");
    }
    if (!s_busy && !s_gave_up && s_n > 1 && s_pos >= 0 && now - s_slide_since >= SLIDE_MS) {
        advance(1);
    }
    if (!lv_obj_has_flag(s_bar, LV_OBJ_FLAG_HIDDEN) && now - s_bar_since >= BAR_HIDE_MS) {
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------------------------------------------------------------- touch */
static void on_screen(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_gestured = false;
    } else if (code == LV_EVENT_GESTURE) {
        lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
        if (d == LV_DIR_LEFT || d == LV_DIR_RIGHT) {
            s_gestured = true;
            if (!s_busy && !s_gave_up && s_n > 1) {
                advance(d == LV_DIR_LEFT ? 1 : -1);
            }
        }
    } else if (code == LV_EVENT_CLICKED && !s_gestured) {
        /* right half: next, left half: previous; the bar shows where you are */
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        if (!s_busy && !s_gave_up && s_n > 1) {
            advance(p.x >= s_dw / 2 ? 1 : -1);
        }
        lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        s_bar_since = now_ms();
    }
}

/* Taps on the visible bar stay there (no picture change) and keep it up. */
static void on_bar(lv_event_t *e)
{
    s_bar_since = now_ms();
}

/* ---------------------------------------------------------------- build */
void gallery_create(lv_obj_t *scr, lv_event_cb_t back_cb)
{
    s_scr = scr;
    s_dw = lv_display_get_horizontal_resolution(NULL);
    s_dh = lv_display_get_vertical_resolution(NULL);
    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_aligned_calloc(64, (size_t)s_dw * s_dh, sizeof(uint16_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    s_req = xQueueCreate(2, sizeof(req_t));
    s_res = xQueueCreate(2, sizeof(res_t));

    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, on_screen, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, on_screen, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(scr, on_screen, LV_EVENT_CLICKED, NULL);

    s_img = lv_image_create(scr);
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_pos(s_img, 0, 0);                     /* always a full-screen buffer */

    s_msg = lv_label_create(scr);
    lv_obj_set_width(s_msg, LV_PCT(80));
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_msg, &font_ui_20, 0);
    lv_obj_set_style_text_color(s_msg, C_MUTED, 0);
    lv_obj_center(s_msg);
    lv_label_set_text(s_msg, buffers_ok() ? "Loading..." : "Not enough memory for the gallery");

    /* top bar: back, file name, position; shown on tap, hides after 4 s */
    s_bar = lv_obj_create(scr);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(s_bar, 16, 0);
    lv_obj_set_style_pad_bottom(s_bar, 40, 0);
    lv_obj_set_style_pad_column(s_bar, 16, 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(s_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_main_opa(s_bar, LV_OPA_80, 0);
    lv_obj_set_style_bg_grad_opa(s_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_grad_dir(s_bar, LV_GRAD_DIR_VER, 0);
    lv_obj_set_flex_flow(s_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_GESTURE_BUBBLE);   /* swipes pass, taps don't */
    lv_obj_add_event_cb(s_bar, on_bar, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b = lv_button_create(s_bar);
    lv_obj_set_size(b, 52, 52);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, C_ROW_SEL, 0);
    lv_obj_add_event_cb(b, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(bl, C_TEXT, 0);
    lv_obj_center(bl);
    s_name = lv_label_create(s_bar);
    lv_obj_set_flex_grow(s_name, 1);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_name, &emoji_font_20, 0);
    lv_obj_set_style_text_color(s_name, C_TEXT, 0);
    s_count = lv_label_create(s_bar);
    lv_obj_set_style_text_font(s_count, &font_ui_16, 0);
    lv_obj_set_style_text_color(s_count, C_MUTED, 0);
    show_bar_text();
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);

    if (buffers_ok() && s_req && s_res) {
        /* 16 KB stack in PSRAM: stb_image + FatFs LFN need room, and internal
         * RAM is what the SD driver runs on. The worker never touches flash. */
        if (xTaskCreatePinnedToCoreWithCaps(worker, "gallery", 16384, NULL, 1, NULL,
                                            tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
            xTaskCreate(worker, "gallery", 8192, NULL, 1, NULL);
        }
    }
    s_timer = lv_timer_create(tick, 10, NULL);      /* GIF frame timing resolution */
    lv_timer_pause(s_timer);
}

void gallery_set_shown(bool shown)
{
    if (shown == s_shown || !s_timer) {
        return;
    }
    s_shown = shown;
    if (!buffers_ok() || !s_req) {
        return;
    }
    if (shown) {
        s_slide_since = now_ms();
        s_next_frame = 0;
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        request_scan();                              /* pick up new or removed files */
        lv_timer_resume(s_timer);
    } else {
        lv_timer_pause(s_timer);
    }
}
