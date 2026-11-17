/*
 * MP3 player engine. One FreeRTOS task owns the decoder and the speaker while
 * a track plays; everything else talks to it through a command queue and reads
 * a status snapshot. The SD card is only ever read here (the USB drive may be
 * writing to it), see bsp_sdcard_mount().
 */
#include "mp3_player.h"
#include "bsp/bsp_board.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mp3dec.h"
#include "id3.h"
#include "ogg_opus.h"
#include "spectrum.h"
#include "esp_heap_caps.h"
#include "esp_random.h"

static const char *TAG = "mp3_player";

/* Library data (paths, tags, play order) lives in PSRAM: thousands of small
 * mallocs would otherwise fill internal RAM, which the SD driver and Wi-Fi
 * need for DMA. */
#define MAX_TRACKS     5000
#define PS_CAPS        (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define MAX_DEPTH      4
#define INBUF_SIZE     (MAINBUF_SIZE * 4)
#define PCM_SAMPLES    (MAX_NSAMP * MAX_NGRAN * MAX_NCHAN)

typedef enum { CMD_PLAY, CMD_TOGGLE, CMD_NEXT, CMD_PREV, CMD_STOP, CMD_SEEK } cmd_type_t;
typedef struct { cmd_type_t type; int arg; } cmd_t;

/* Returned by command handling / play_file(): a track index, or one of these */
#define ACT_CONTINUE  (-2)
#define ACT_STOP      (-1)
#define ACT_END       (-3)      /* play_file(): track finished normally */
#define ACT_FAIL      (-4)      /* play_file(): track could not be played */

static QueueHandle_t s_cmds;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static mp3_status_t s_status = { .state = MP3_STOPPED, .track = -1 };
static volatile bool s_busy;            /* task is inside play_file() */

static char **s_paths;                  /* full paths, sorted */
static int s_count;

/* ID3 metadata, filled in by a background task after each scan */
typedef struct {
    char *title, *artist, *album;       /* NULL if absent */
    bool ready;                          /* written with release / read with acquire */
} meta_t;
static meta_t *s_meta;
static TaskHandle_t s_id3_task;
static volatile bool s_id3_cancel, s_id3_busy;
static volatile uint32_t s_meta_serial;

/* Play order: s_order[k] = track at position k, s_pos[track] = its position.
 * Rebuilt only by the player task (or by scan while the task is idle). */
static volatile bool s_shuffle;
static volatile mp3_repeat_t s_repeat = MP3_REPEAT_OFF;
static int *s_order, *s_pos;
static int s_order_n = -1;              /* track count the order was built for */
static bool s_order_shuffled;

static uint8_t s_inbuf[INBUF_SIZE];
static int16_t s_pcm[PCM_SAMPLES];
static int16_t s_stereo[PCM_SAMPLES];

/* -------------------------------------------------------------- playlist */
static void free_playlist(void)
{
    for (int i = 0; i < s_count; i++) {
        free(s_paths[i]);
        if (s_meta) {
            free(s_meta[i].title);
            free(s_meta[i].artist);
            free(s_meta[i].album);
            memset(&s_meta[i], 0, sizeof(s_meta[i]));
        }
    }
    s_count = 0;
}

static char *dup_field(const char *s)
{
    if (!s[0]) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *d = heap_caps_malloc(n, PS_CAPS);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

static bool meta_ready(int i)
{
    return s_meta && i >= 0 && i < s_count && __atomic_load_n(&s_meta[i].ready, __ATOMIC_ACQUIRE);
}

static bool is_opus(const char *name);

static void id3_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (int i = 0; i < s_count && !s_id3_cancel; i++) {
            FILE *f = fopen(s_paths[i], "rb");
            if (f) {
                id3_info_t info;
                bool found = is_opus(s_paths[i]) ? opus_file_tags(f, &info) : id3_read(f, &info);
                if (found) {
                    s_meta[i].title = dup_field(info.title);
                    s_meta[i].artist = dup_field(info.artist);
                    s_meta[i].album = dup_field(info.album);
                }
                fclose(f);
            }
            __atomic_store_n(&s_meta[i].ready, true, __ATOMIC_RELEASE);
            s_meta_serial++;
        }
        s_id3_busy = false;
    }
}

static bool has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static bool is_opus(const char *name)
{
    return has_ext(name, ".opus");
}

static bool is_audio(const char *name)
{
    return name[0] != '.' && (has_ext(name, ".mp3") || is_opus(name));
}

static void scan_dir(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_count < MAX_TRACKS) {
        if (e->d_name[0] == '.' ) {
            continue;                       /* ., .., macOS ._ files, hidden */
        }
        size_t len = strlen(dir) + 1 + strlen(e->d_name) + 1;
        char *path = heap_caps_malloc(len, PS_CAPS);
        if (!path) {
            break;
        }
        snprintf(path, len, "%s/%s", dir, e->d_name);
        if (e->d_type == DT_DIR) {
            if (depth < MAX_DEPTH && strcasecmp(e->d_name, "System Volume Information") != 0) {
                scan_dir(path, depth + 1);
            }
            free(path);
        } else if (is_audio(e->d_name)) {
            s_paths[s_count++] = path;
        } else {
            free(path);
        }
    }
    closedir(d);
}

static int cmp_path(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

int mp3_player_count(void)
{
    return s_count;
}

bool mp3_player_meta_ready(int i)
{
    return meta_ready(i);
}

uint32_t mp3_player_meta_serial(void)
{
    return s_meta_serial;
}

const char *mp3_player_artist(int i)
{
    return meta_ready(i) && s_meta[i].artist ? s_meta[i].artist : "";
}

const char *mp3_player_album(int i)
{
    return meta_ready(i) && s_meta[i].album ? s_meta[i].album : "";
}

const char *mp3_player_title(int i)
{
    static char buf[128];
    if (i < 0 || i >= s_count) {
        return "";
    }
    if (meta_ready(i) && s_meta[i].title) {
        return s_meta[i].title;              /* ID3 title */
    }
    const char *base = strrchr(s_paths[i], '/');
    base = base ? base + 1 : s_paths[i];
    const char *dot = strrchr(base, '.');
    size_t n = dot ? (size_t)(dot - base) : strlen(base);   /* drop the extension */
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    memcpy(buf, base, n);
    buf[n] = '\0';
    return buf;
}

const char *mp3_player_folder(int i)
{
    static char buf[128];
    if (i < 0 || i >= s_count) {
        return "";
    }
    /* relative to the Music folder: "Album" for /sd/Music/Album/x.mp3, "" for /sd/Music/x.mp3 */
    const char *rel = s_paths[i] + strlen(BSP_SD_MOUNT_POINT "/" MP3_MUSIC_DIR) + 1;
    const char *slash = strrchr(rel, '/');
    size_t n = slash ? (size_t)(slash - rel) : 0;
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    memcpy(buf, rel, n);
    buf[n] = '\0';
    return buf;
}

/* ---------------------------------------------------------------- status */
static void set_status(mp3_state_t st, int track)
{
    portENTER_CRITICAL(&s_lock);
    if (track != s_status.track || st == MP3_STOPPED) {
        s_status.serial++;
    }
    s_status.state = st;
    s_status.track = track;
    if (st == MP3_STOPPED) {
        s_status.pos_ms = 0;
    }
    portEXIT_CRITICAL(&s_lock);
}

void mp3_player_get_status(mp3_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

/* -------------------------------------------------------------- commands */
static void send(cmd_type_t t, int arg)
{
    cmd_t c = { t, arg };
    if (s_cmds) {
        xQueueSend(s_cmds, &c, pdMS_TO_TICKS(100));
    }
}

void mp3_player_play(int index) { send(CMD_PLAY, index); }
void mp3_player_toggle(void)    { send(CMD_TOGGLE, 0); }
void mp3_player_next(void)      { send(CMD_NEXT, 0); }
void mp3_player_prev(void)      { send(CMD_PREV, 0); }
void mp3_player_stop(void)      { send(CMD_STOP, 0); }
void mp3_player_seek(int permille)
{
    send(CMD_SEEK, permille < 0 ? 0 : (permille > 1000 ? 1000 : permille));
}

void mp3_player_set_shuffle(bool on)          { s_shuffle = on; }
bool mp3_player_get_shuffle(void)             { return s_shuffle; }
void mp3_player_set_repeat(mp3_repeat_t mode) { s_repeat = mode; }
mp3_repeat_t mp3_player_get_repeat(void)      { return s_repeat; }

/* ----------------------------------------------------------- play order */
static void set_order_pos(void)
{
    for (int k = 0; k < s_count; k++) {
        s_pos[s_order[k]] = k;
    }
}

/* Identity order, or a fresh shuffle that starts with `first` (if >= 0). */
static void build_order(int first)
{
    for (int k = 0; k < s_count; k++) {
        s_order[k] = k;
    }
    if (s_shuffle) {
        for (int k = s_count - 1; k > 0; k--) {          /* Fisher-Yates */
            int j = (int)(esp_random() % (uint32_t)(k + 1));
            int t = s_order[k]; s_order[k] = s_order[j]; s_order[j] = t;
        }
        if (first >= 0 && first < s_count) {
            for (int k = 0; k < s_count; k++) {
                if (s_order[k] == first) {
                    s_order[k] = s_order[0];
                    s_order[0] = first;
                    break;
                }
            }
        }
    }
    set_order_pos();
    s_order_n = s_count;
    s_order_shuffled = s_shuffle;
}

/* Rebuild if the playlist or the shuffle switch changed. Turning shuffle on
 * mid-track shuffles the rest of the list after the current track. */
static void ensure_order(int cur)
{
    if (s_order_n != s_count || s_order_shuffled != s_shuffle) {
        build_order(cur);
    }
}

static int next_of(int idx)                 /* manual "next": always wraps */
{
    ensure_order(idx);
    return idx < 0 ? s_order[0] : s_order[(s_pos[idx] + 1) % s_count];
}

static int prev_of(int idx)
{
    ensure_order(idx);
    return idx < 0 ? s_order[s_count - 1] : s_order[(s_pos[idx] + s_count - 1) % s_count];
}

/* What plays after `idx` ends by itself (or fails to play). */
static int auto_next(int idx, bool failed)
{
    if (!failed && s_repeat == MP3_REPEAT_ONE) {
        return idx;
    }
    ensure_order(idx);
    int k = s_pos[idx] + 1;
    if (k < s_count) {
        return s_order[k];
    }
    if (s_repeat == MP3_REPEAT_OFF) {
        return ACT_STOP;                         /* end of the list */
    }
    if (s_shuffle) {                             /* new round, new order */
        build_order(-1);
        if (s_count > 1 && s_order[0] == idx) {  /* don't play the same track twice in a row */
            s_order[0] = s_order[1];
            s_order[1] = idx;
            set_order_pos();
        }
    }
    return s_order[0];
}

/* Maps a command to the next action. idx/pos describe the current track
 * (idx = -1 when stopped). Toggle is handled by the caller. */
static int resolve(const cmd_t *c, int idx, uint32_t pos_ms)
{
    if (s_count == 0) {
        return ACT_STOP;
    }
    switch (c->type) {
    case CMD_PLAY:
        return (c->arg >= 0 && c->arg < s_count) ? c->arg : ACT_CONTINUE;
    case CMD_NEXT:
        return next_of(idx);
    case CMD_PREV:
        if (idx >= 0 && pos_ms > 3000) {
            return idx;
        }
        return prev_of(idx);
    case CMD_STOP:
        return ACT_STOP;
    default:
        return ACT_CONTINUE;
    }
}

/* ------------------------------------------------------------- decoding */
static long skip_id3v2(FILE *f)
{
    uint8_t h[10];
    if (fread(h, 1, sizeof(h), f) == sizeof(h) && memcmp(h, "ID3", 3) == 0) {
        long size = ((long)(h[6] & 0x7F) << 21) | ((h[7] & 0x7F) << 14) | ((h[8] & 0x7F) << 7) | (h[9] & 0x7F);
        size += 10 + ((h[5] & 0x10) ? 10 : 0);   /* header (+ footer) */
        fseek(f, size, SEEK_SET);
        return size;
    }
    fseek(f, 0, SEEK_SET);
    return 0;
}

/* VBR info from a Xing/Info header in the first frame (LAME and most encoders
 * write one). Gives an exact duration and a 100-point seek table. */
typedef struct {
    uint32_t dur_ms;        /* 0 if unknown */
    bool has_toc;
    uint8_t toc[100];
} vbr_info_t;

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void read_vbr_info(FILE *f, long audio_start, HMP3Decoder dec, vbr_info_t *v)
{
    memset(v, 0, sizeof(*v));
    size_t n = fread(s_inbuf, 1, 2048, f);
    fseek(f, audio_start, SEEK_SET);
    int off = MP3FindSyncWord(s_inbuf, (int)n);
    MP3FrameInfo fi;
    if (off < 0 || MP3GetNextFrameInfo(dec, &fi, s_inbuf + off) != ERR_MP3_NONE || fi.samprate <= 0 || fi.nChans <= 0) {
        return;
    }
    /* tag sits after the side info: 13..36 bytes into the frame depending on version/channels */
    for (size_t i = off + 4; i + 8 <= n && i < (size_t)off + 4 + 40; i++) {
        if (memcmp(s_inbuf + i, "Xing", 4) != 0 && memcmp(s_inbuf + i, "Info", 4) != 0) {
            continue;
        }
        uint32_t flags = be32(s_inbuf + i + 4);
        size_t p = i + 8;
        if ((flags & 0x1) && p + 4 <= n) {               /* frame count */
            uint32_t frames = be32(s_inbuf + p);
            uint32_t spf = fi.outputSamps / fi.nChans;   /* 1152 or 576 */
            v->dur_ms = (uint32_t)((uint64_t)frames * spf * 1000 / fi.samprate);
            p += 4;
        }
        if (flags & 0x2) {                               /* byte count */
            p += 4;
        }
        if ((flags & 0x4) && p + 100 <= n) {             /* seek table */
            memcpy(v->toc, s_inbuf + p, 100);
            v->has_toc = true;
        }
        return;
    }
}

/* Byte offset (from audio_start) for a position in permille of the track. */
static long seek_offset(const vbr_info_t *v, long audio_bytes, int permille)
{
    if (!v->has_toc) {
        return (long)((int64_t)audio_bytes * permille / 1000);
    }
    float pct = permille / 10.0f;
    int a = (int)pct;
    if (a > 99) {
        a = 99;
    }
    float fa = v->toc[a];
    float fb = a < 99 ? v->toc[a + 1] : 256.0f;
    float fx = fa + (fb - fa) * (pct - a);
    return (long)(fx / 256.0f * audio_bytes);
}

/* ---- fades: a user's track change fades the old track out, the new one in.
 * Natural track-to-track transitions stay instant (gapless albums). The gain
 * is applied to the PCM just before the speaker (and the analyzer). */
#define FADE_OUT_MS 300
#define FADE_IN_MS  400
#define GAIN_ONE    65536
static int32_t s_gain = GAIN_ONE;       /* player task only */
static int s_fade_dir;                  /* -1 fading out, +1 fading in, 0 steady */
static int s_fade_ms;

static void fade_start(int dir, int ms)
{
    s_fade_dir = dir;
    s_fade_ms = ms;
}

static void apply_gain(int16_t *b, int frames, uint32_t rate)
{
    if (s_fade_dir == 0 && s_gain == GAIN_ONE) {
        return;
    }
    int32_t step = 0;
    if (s_fade_dir && rate) {
        step = (int32_t)((int64_t)GAIN_ONE * 1000 / ((int64_t)rate * s_fade_ms));
        if (step < 1) {
            step = 1;
        }
    }
    for (int i = 0; i < frames; i++) {
        b[2 * i] = (int16_t)(((int32_t)b[2 * i] * s_gain) >> 16);
        b[2 * i + 1] = (int16_t)(((int32_t)b[2 * i + 1] * s_gain) >> 16);
        if (s_fade_dir) {
            s_gain += s_fade_dir * step;
            if (s_gain <= 0) {
                s_gain = 0;
                s_fade_dir = 0;                     /* out: stays silent until the switch */
            } else if (s_gain >= GAIN_ONE) {
                s_gain = GAIN_ONE;
                s_fade_dir = 0;
            }
        }
    }
}

/* Transport shared by all formats: pause, seek requests and track changes. */
typedef struct {
    int idx;
    bool paused;
    int seek;                    /* permille requested, -1 = none */
    int pending;                 /* track change waiting for the fade-out, ACT_CONTINUE = none */
    bool audible;                /* some audio has gone out (else switch without fading) */
} transport_t;

static void transport_init(transport_t *t, int idx, bool fade_in)
{
    memset(t, 0, sizeof(*t));
    t->idx = idx;
    t->pending = ACT_CONTINUE;
    if (fade_in) {
        s_gain = 0;
        fade_start(+1, FADE_IN_MS);
    } else {
        s_gain = GAIN_ONE;
        fade_start(0, 0);
    }
}

/* Non-blocking while playing, blocking while paused. Returns ACT_CONTINUE
 * (check t->seek) or the action of a track-changing command. */
static int poll_commands(transport_t *t)
{
    t->seek = -1;
    if (t->pending != ACT_CONTINUE && s_gain == 0) {
        return t->pending;                          /* faded out: switch now */
    }
    cmd_t c;
    while (xQueueReceive(s_cmds, &c, t->paused ? portMAX_DELAY : 0) == pdTRUE) {
        if (c.type == CMD_TOGGLE) {
            if (t->pending != ACT_CONTINUE) {
                continue;                           /* already on the way out */
            }
            t->paused = !t->paused;
            set_status(t->paused ? MP3_PAUSED : MP3_PLAYING, t->idx);
        } else if (c.type == CMD_SEEK) {
            if (t->pending != ACT_CONTINUE) {
                continue;
            }
            t->seek = c.arg;
            return ACT_CONTINUE;
        } else {
            int a = resolve(&c, t->idx, s_status.pos_ms);
            if (a != ACT_CONTINUE) {
                if (t->paused || !t->audible || s_gain == 0) {
                    return a;                       /* silent anyway: no fade needed */
                }
                t->pending = a;                     /* a newer choice replaces an older one */
                fade_start(-1, FADE_OUT_MS);
                return ACT_CONTINUE;                /* keep playing while it fades */
            }
        }
        if (!t->paused) {
            break;
        }
    }
    return ACT_CONTINUE;
}

static int play_mp3(int idx, bool fade_in)
{
    const char *path = s_paths[idx];
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return ACT_FAIL;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    long audio_start = skip_id3v2(f);
    long audio_bytes = file_size - audio_start;

    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) {
        fclose(f);
        ESP_LOGE(TAG, "decoder alloc failed");
        return ACT_STOP;
    }

    vbr_info_t vbr;
    read_vbr_info(f, audio_start, dec, &vbr);

    ESP_LOGI(TAG, "playing %s", path);
    portENTER_CRITICAL(&s_lock);
    s_status.pos_ms = 0;
    s_status.dur_ms = vbr.dur_ms;
    s_status.codec = MP3_CODEC_MP3;
    portEXIT_CRITICAL(&s_lock);
    set_status(MP3_PLAYING, idx);

    uint8_t *rp = s_inbuf;
    int left = 0;
    bool eof = false;
    transport_t tp;
    transport_init(&tp, idx, fade_in);
    uint64_t frames_out = 0;
    uint32_t rate = 0;
    int bad = 0;
    bool failed = false;
    int action = ACT_CONTINUE;

    while (action == ACT_CONTINUE) {
        action = poll_commands(&tp);
        if (action != ACT_CONTINUE) {
            break;
        }
        if (tp.seek >= 0) {
            {
                uint32_t dur = s_status.dur_ms;
                if (rate && dur) {
                    long off = seek_offset(&vbr, audio_bytes, tp.seek);
                    fseek(f, audio_start + off, SEEK_SET);
                    rp = s_inbuf;
                    left = 0;
                    eof = false;
                    /* fresh decoder: drop the old bit reservoir / overlap */
                    MP3FreeDecoder(dec);
                    dec = MP3InitDecoder();
                    if (!dec) {
                        action = ACT_STOP;
                        break;
                    }
                    uint32_t pos = (uint32_t)((uint64_t)dur * tp.seek / 1000);
                    frames_out = (uint64_t)pos * rate / 1000;
                    portENTER_CRITICAL(&s_lock);
                    s_status.pos_ms = pos;
                    portEXIT_CRITICAL(&s_lock);
                    ESP_LOGI(TAG, "seek to %lu ms (byte %ld)", (unsigned long)pos, off);
                }
            }
            if (action != ACT_CONTINUE) {
                break;                               /* decoder re-alloc failed */
            }
            continue;
        }

        /* keep the input buffer topped up */
        if (left < MAINBUF_SIZE && !eof) {
            memmove(s_inbuf, rp, left);
            size_t n = fread(s_inbuf + left, 1, INBUF_SIZE - left, f);
            left += n;
            rp = s_inbuf;
            eof = (n == 0);
        }
        if (left <= 0) {
            break;                                   /* end of track */
        }

        int off = MP3FindSyncWord(rp, left);
        if (off < 0) {
            left = 0;                                /* no frame in buffer, read more */
            continue;
        }
        rp += off;
        left -= off;

        int err = MP3Decode(dec, &rp, &left, s_pcm, 0);
        if (err == ERR_MP3_INDATA_UNDERFLOW) {
            if (eof) {
                break;
            }
            if (left >= MAINBUF_SIZE) {             /* buffer full but frame incomplete: skip */
                rp++;
                left--;
            } else {
                memmove(s_inbuf, rp, left);         /* force a refill next round */
                rp = s_inbuf;
                size_t n = fread(s_inbuf + left, 1, INBUF_SIZE - left, f);
                left += n;
                eof = (n == 0);
            }
            continue;
        }
        if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
            continue;                                /* needs the next frame's bit reservoir */
        }
        if (err != ERR_MP3_NONE) {
            if (++bad > 200) {
                ESP_LOGW(TAG, "too many decode errors (%d), skipping file", err);
                failed = true;
                break;
            }
            if (left > 0) {                          /* resync one byte further */
                rp++;
                left--;
            }
            continue;
        }
        bad = 0;

        MP3FrameInfo fi;
        MP3GetLastFrameInfo(dec, &fi);
        if ((uint32_t)fi.samprate != rate) {
            if (bsp_audio_set_sample_rate(fi.samprate) != ESP_OK) {
                ESP_LOGE(TAG, "unsupported sample rate %d", fi.samprate);
                failed = true;
                break;
            }
            rate = fi.samprate;
        }

        int frames;
        int16_t *out;
        if (fi.nChans == 1) {
            frames = fi.outputSamps;
            for (int i = 0; i < frames; i++) {
                s_stereo[2 * i] = s_stereo[2 * i + 1] = s_pcm[i];
            }
            out = s_stereo;
        } else {
            frames = fi.outputSamps / 2;
            out = s_pcm;
        }
        apply_gain(out, frames, rate);
        spectrum_feed(out, frames, rate);
        bsp_audio_write(out, frames, portMAX_DELAY);
        frames_out += frames;
        tp.audible = true;

        /* position + duration estimate (byte ratio, so VBR converges too) */
        uint32_t pos = (uint32_t)(frames_out * 1000 / rate);
        long consumed = ftell(f) - left - audio_start;
        uint32_t dur;
        if (vbr.dur_ms) {
            dur = vbr.dur_ms;                        /* exact, from the Xing header */
        } else if (consumed > 65536) {
            dur = (uint32_t)((uint64_t)pos * audio_bytes / consumed);
        } else {
            dur = fi.bitrate > 0 ? (uint32_t)((uint64_t)audio_bytes * 8000 / fi.bitrate) : 0;
        }
        portENTER_CRITICAL(&s_lock);
        s_status.pos_ms = pos;
        s_status.dur_ms = dur;
        s_status.sample_rate = rate;
        s_status.kbps = (uint16_t)(fi.bitrate / 1000);
        s_status.channels = (uint8_t)fi.nChans;
        portEXIT_CRITICAL(&s_lock);
    }

    if (dec) {
        MP3FreeDecoder(dec);
    }
    fclose(f);
    if (tp.pending != ACT_CONTINUE && (action == ACT_CONTINUE || action == ACT_STOP)) {
        action = tp.pending;                         /* track ended mid-fade: still honour it */
    }
    if (action == ACT_CONTINUE) {
        /* ran to the end; a file that produced no audio counts as a failure so
         * repeat-one can't spin on it */
        action = (failed || frames_out == 0) ? ACT_FAIL : ACT_END;
    }
    return action;
}

/* ------------------------------------------------------------------ Opus */
static opus_stream_t s_opus;                         /* ~8.5 KB, one stream at a time */
static int16_t *s_opcm;                              /* OPUS_STREAM_MAX_FRAMES * 2, PSRAM (23 KB) */

static int play_opus(int idx, bool fade_in)
{
    const char *path = s_paths[idx];
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return ACT_FAIL;
    }
    int r = opus_stream_open(&s_opus, f);
    if (r != 0) {
        ESP_LOGE(TAG, "%s: not a playable Ogg Opus file (%d)", path, r);
        opus_stream_close(&s_opus);
        fclose(f);
        return ACT_FAIL;
    }
    if (bsp_audio_set_sample_rate(OPUS_STREAM_RATE) != ESP_OK) {
        opus_stream_close(&s_opus);
        fclose(f);
        return ACT_FAIL;
    }

    const uint64_t total = opus_stream_total(&s_opus);
    const uint32_t dur_ms = (uint32_t)(total / 48);
    ESP_LOGI(TAG, "playing %s (Opus, %d ch, %lu ms)", path, s_opus.head.channels, (unsigned long)dur_ms);
    portENTER_CRITICAL(&s_lock);
    s_status.pos_ms = 0;
    s_status.dur_ms = dur_ms;
    s_status.sample_rate = OPUS_STREAM_RATE;
    s_status.channels = (uint8_t)s_opus.head.channels;
    s_status.kbps = dur_ms ? (uint16_t)((uint64_t)(s_opus.r.file_size - s_opus.data_start) * 8 / dur_ms) : 0;
    s_status.codec = MP3_CODEC_OPUS;
    portEXIT_CRITICAL(&s_lock);
    set_status(MP3_PLAYING, idx);

    transport_t tp;
    transport_init(&tp, idx, fade_in);
    int action;
    bool failed = false;
    uint64_t frames_out = 0;
    for (;;) {
        action = poll_commands(&tp);
        if (action != ACT_CONTINUE) {
            break;
        }
        if (tp.seek >= 0) {
            opus_stream_seek(&s_opus, total * (uint64_t)tp.seek / 1000);
        } else {
            int n = opus_stream_read(&s_opus, s_opcm);
            if (n < 0) {
                ESP_LOGW(TAG, "%s: too many bad packets, skipping", path);
                failed = true;
                break;
            }
            if (n == 0) {
                break;                               /* end of track */
            }
            apply_gain(s_opcm, n, OPUS_STREAM_RATE);
            spectrum_feed(s_opcm, n, OPUS_STREAM_RATE);
            bsp_audio_write(s_opcm, n, portMAX_DELAY);
            frames_out += n;
            tp.audible = true;
        }
        portENTER_CRITICAL(&s_lock);
        s_status.pos_ms = (uint32_t)(opus_stream_pos(&s_opus) / 48);
        portEXIT_CRITICAL(&s_lock);
    }
    opus_stream_close(&s_opus);
    fclose(f);
    if (tp.pending != ACT_CONTINUE && action == ACT_CONTINUE) {
        action = tp.pending;                         /* track ended mid-fade: still honour it */
    }
    if (action == ACT_CONTINUE) {
        action = (failed || frames_out == 0) ? ACT_FAIL : ACT_END;
    }
    return action;
}

static int play_file(int idx, bool fade_in)
{
    return is_opus(s_paths[idx]) ? play_opus(idx, fade_in) : play_mp3(idx, fade_in);
}

static void player_task(void *arg)
{
    int next = ACT_STOP;
    for (;;) {
        if (next < 0) {
            set_status(MP3_STOPPED, s_status.track);
            s_busy = false;
            cmd_t c;
            xQueueReceive(s_cmds, &c, portMAX_DELAY);
            if (c.type == CMD_TOGGLE) {
                next = !s_count ? ACT_STOP
                     : (s_status.track >= 0 && s_status.track < s_count) ? s_status.track
                     : next_of(-1);                  /* first in (possibly shuffled) order */
            } else {
                next = resolve(&c, -1, 0);
            }
            continue;
        }
        s_busy = true;
        if (!bsp_audio_acquire(3000)) {
            ESP_LOGW(TAG, "speaker busy");
            next = ACT_STOP;
            continue;
        }
        int fails = 0;
        bool fade_in = true;                         /* started by the user */
        while (next >= 0) {
            int cur = next;
            int r = play_file(cur, fade_in);
            if (r == ACT_END || r == ACT_FAIL) {
                fails = (r == ACT_FAIL) ? fails + 1 : 0;
                /* every track failing in a row: stop instead of looping forever */
                next = (fails >= s_count) ? ACT_STOP : auto_next(cur, r == ACT_FAIL);
                fade_in = false;                     /* natural transition: gapless */
            } else {
                next = r;
                fade_in = true;                      /* the user picked it */
            }
        }
        bsp_audio_release();
    }
}

/* ------------------------------------------------------------------ API */
int mp3_player_scan(void)
{
    /* stop and wait until the task has closed the file */
    if (s_cmds) {
        xQueueReset(s_cmds);
        send(CMD_STOP, 0);
        for (int i = 0; i < 100 && s_busy; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    /* stop the tag reader before freeing what it is working on */
    s_id3_cancel = true;
    for (int i = 0; i < 300 && s_id3_busy; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_id3_cancel = false;

    free_playlist();
    set_status(MP3_STOPPED, -1);

    if (bsp_sdcard_mount() != ESP_OK) {
        return -1;
    }
    if (!s_paths) {
        s_paths = heap_caps_calloc(MAX_TRACKS, sizeof(char *), PS_CAPS);
        s_meta = heap_caps_calloc(MAX_TRACKS, sizeof(meta_t), PS_CAPS);
        s_order = heap_caps_calloc(MAX_TRACKS, sizeof(int), PS_CAPS);
        s_pos = heap_caps_calloc(MAX_TRACKS, sizeof(int), PS_CAPS);
        if (!s_paths || !s_meta || !s_order || !s_pos) {
            return -1;
        }
    }
    scan_dir(BSP_SD_MOUNT_POINT "/" MP3_MUSIC_DIR, 0);   /* missing folder: 0 tracks */
    qsort(s_paths, s_count, sizeof(char *), cmp_path);
    build_order(-1);                         /* task is idle here, safe to touch */
    ESP_LOGI(TAG, "%d MP3 file(s) found", s_count);

    if (s_count && s_id3_task) {
        s_id3_busy = true;                   /* set before waking, so a quick rescan waits */
        xTaskNotifyGive(s_id3_task);
    }
    return s_count;
}

esp_err_t mp3_player_init(void)
{
    if (s_cmds) {
        return ESP_OK;
    }
    s_cmds = xQueueCreate(4, sizeof(cmd_t));
    ESP_RETURN_ON_FALSE(s_cmds, ESP_ERR_NO_MEM, TAG, "queue alloc failed");
    s_opcm = heap_caps_malloc(OPUS_STREAM_MAX_FRAMES * 2 * sizeof(int16_t), PS_CAPS);
    ESP_RETURN_ON_FALSE(s_opcm, ESP_ERR_NO_MEM, TAG, "opus pcm alloc failed");
    /* 32 KB: libopus (VAR_ARRAYS build) keeps its decode scratch on the stack */
    BaseType_t ok = xTaskCreate(player_task, "player", 32768, NULL, 6, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    /* lowest useful priority (same as the Arduino loop/LVGL task, time-sliced);
     * far below the player so tag reading never causes audio dropouts */
    ok = xTaskCreate(id3_task, "id3", 6144, NULL, 1, &s_id3_task);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "id3 task create failed");
    return ESP_OK;
}
