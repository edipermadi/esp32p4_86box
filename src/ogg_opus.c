/*
 * Minimal Ogg demuxer + Opus header/tag parser (see ogg_opus.h).
 * CRCs are not checked: pages are validated by capture pattern, version and
 * stream serial, which is enough for files on an SD card.
 */
#include "ogg_opus.h"
#include <stddef.h>
#include <string.h>
#include <strings.h>

#define PAGE_HDR 27

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int64_t le64(const uint8_t *p)  { return (int64_t)((uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32); }

void ogg_init(ogg_reader_t *r, FILE *f, long file_size)
{
    memset(r, 0, sizeof(*r));
    r->f = f;
    r->file_size = file_size;
    r->page_granule = -1;
}

/* Parses the page header at the current file position. Returns 1 for a page
 * of our stream, 0 for another stream (already skipped), -1 at EOF / garbage. */
static int read_page(ogg_reader_t *r)
{
    uint8_t h[PAGE_HDR];
    long off = ftell(r->f);
    if (fread(h, 1, PAGE_HDR, r->f) != PAGE_HDR || memcmp(h, "OggS", 4) != 0 || h[4] != 0) {
        return -1;
    }
    uint32_t serial = le32(h + 14);
    int nseg = h[26];
    uint8_t lacing[255];
    if (fread(lacing, 1, nseg, r->f) != (size_t)nseg) {
        return -1;
    }
    long body = 0;
    for (int i = 0; i < nseg; i++) {
        body += lacing[i];
    }
    if (!r->have_serial) {
        r->serial = serial;
        r->have_serial = true;
    }
    r->next_page_off = off + PAGE_HDR + nseg + body;
    if (serial != r->serial) {
        fseek(r->f, r->next_page_off, SEEK_SET);
        return 0;
    }
    memcpy(r->lacing, lacing, nseg);
    r->nseg = nseg;
    r->seg = 0;
    r->seg_left = 0;
    r->page_granule = le64(h + 6);
    r->page_flags = h[5];
    return 1;
}

static bool next_page(ogg_reader_t *r)
{
    for (;;) {
        int k = read_page(r);
        if (k == 1) {
            return true;
        }
        if (k < 0) {
            r->eof = true;
            return false;
        }
    }
}

/* Reads (dst != NULL) or skips up to n bytes of the current packet. */
static int packet_bytes(ogg_reader_t *r, uint8_t *dst, int n)
{
    int got = 0;
    while (n > 0 && !r->pkt_end) {
        if (r->seg_left == 0) {
            if (r->seg >= r->nseg) {
                if (!next_page(r)) {
                    r->pkt_end = true;
                    break;
                }
                continue;
            }
            r->last_lace = r->lacing[r->seg++];
            r->seg_left = r->last_lace;
            if (r->seg_left == 0) {               /* zero-length terminating segment */
                r->pkt_end = true;
                break;
            }
        }
        int k = n < r->seg_left ? n : r->seg_left;
        if (dst) {
            if (fread(dst + got, 1, k, r->f) != (size_t)k) {
                r->eof = r->pkt_end = true;
                break;
            }
        } else {
            fseek(r->f, k, SEEK_CUR);
        }
        got += k;
        n -= k;
        r->seg_left -= k;
        if (r->seg_left == 0 && r->last_lace < 255) {
            r->pkt_end = true;
        }
    }
    return got;
}

/* Finishes the current packet and moves to the start of the next one. */
static bool packet_begin(ogg_reader_t *r)
{
    if (r->in_packet) {
        while (!r->pkt_end) {
            packet_bytes(r, NULL, 1 << 30);
        }
    }
    r->in_packet = true;
    r->pkt_end = false;
    while (r->seg >= r->nseg && r->seg_left == 0) {
        if (!next_page(r)) {
            return false;
        }
    }
    return true;
}

int ogg_read_packet(ogg_reader_t *r, uint8_t *buf, int cap)
{
    if (!packet_begin(r)) {
        return -1;
    }
    int n = packet_bytes(r, buf, cap);
    if (!r->pkt_end) {                            /* bigger than the buffer: drop it */
        while (!r->pkt_end) {
            packet_bytes(r, NULL, 1 << 30);
        }
        return -2;
    }
    return n;
}

/* --------------------------------------------------------------- headers */
static bool read_u32(ogg_reader_t *r, uint32_t *v)
{
    uint8_t b[4];
    if (packet_bytes(r, b, 4) != 4) {
        return false;
    }
    *v = le32(b);
    return true;
}

static void take_tag(ogg_reader_t *r, uint32_t len, id3_info_t *t)
{
    static const struct { const char *key; size_t off; } keys[] = {
        { "TITLE=", offsetof(id3_info_t, title) },
        { "ARTIST=", offsetof(id3_info_t, artist) },
        { "ALBUM=", offsetof(id3_info_t, album) },
    };
    uint8_t head[8];
    uint32_t n = len < sizeof(head) ? len : sizeof(head);
    packet_bytes(r, head, (int)n);
    len -= n;
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        size_t kl = strlen(keys[k].key);
        char *dst = (char *)t + keys[k].off;
        if (n >= kl && strncasecmp((const char *)head, keys[k].key, kl) == 0 && !dst[0]) {
            /* value = the rest of head after the key, then the remaining bytes */
            size_t have = n - kl, cap = ID3_FIELD_MAX - 1, used = 0;
            memcpy(dst, head + kl, have < cap ? have : cap);
            used = have < cap ? have : cap;
            uint32_t more = len < cap - used ? len : (uint32_t)(cap - used);
            used += packet_bytes(r, (uint8_t *)dst + used, (int)more);
            len -= more;
            dst[used] = '\0';
            /* don't leave half a UTF-8 sequence if the value was cut */
            if (len > 0) {
                while (used > 0 && ((uint8_t)dst[used - 1] & 0xC0) == 0x80) {
                    dst[--used] = '\0';
                }
                if (used > 0 && ((uint8_t)dst[used - 1] & 0x80)) {
                    dst[--used] = '\0';
                }
            }
            break;
        }
    }
    packet_bytes(r, NULL, (int)len);              /* skip the rest (e.g. cover art) */
}

bool opus_read_headers(ogg_reader_t *r, opus_head_t *h, id3_info_t *tags)
{
    uint8_t b[19];
    if (!packet_begin(r) || packet_bytes(r, b, 19) != 19 || memcmp(b, "OpusHead", 8) != 0 || (b[8] >> 4) != 0) {
        return false;                             /* not Opus (e.g. Vorbis in .ogg) */
    }
    h->channels = b[9];
    h->preskip = b[10] | b[11] << 8;
    h->gain_q8 = (int16_t)(b[16] | b[17] << 8);
    h->family = b[18];

    uint8_t m[8];
    if (!packet_begin(r) || packet_bytes(r, m, 8) != 8 || memcmp(m, "OpusTags", 8) != 0) {
        return false;
    }
    if (tags) {
        memset(tags, 0, sizeof(*tags));
        uint32_t vlen, count;
        if (read_u32(r, &vlen)) {
            packet_bytes(r, NULL, (int)vlen);     /* vendor string */
            if (read_u32(r, &count)) {
                for (uint32_t i = 0; i < count && !r->pkt_end; i++) {
                    uint32_t len;
                    if (!read_u32(r, &len)) {
                        break;
                    }
                    take_tag(r, len, tags);
                }
            }
        }
    }
    /* finish the tags packet; audio starts on the next page (RFC 7845 sec. 3) */
    while (!r->pkt_end) {
        packet_bytes(r, NULL, 1 << 30);
    }
    return true;
}

/* ------------------------------------------------------------ page search */
/* Finds the first page of our stream starting at or after `from`.
 * Returns its offset and granule, or -1. Leaves the file position undefined. */
static long find_page(ogg_reader_t *r, long from, int64_t *granule)
{
    uint8_t buf[512 + PAGE_HDR];
    long pos = from;
    while (pos < r->file_size) {
        fseek(r->f, pos, SEEK_SET);
        size_t n = fread(buf, 1, sizeof(buf), r->f);
        if (n < PAGE_HDR) {
            return -1;
        }
        for (size_t i = 0; i + PAGE_HDR <= n; i++) {
            if (buf[i] == 'O' && memcmp(buf + i, "OggS", 4) == 0 && buf[i + 4] == 0 &&
                le32(buf + i + 14) == r->serial) {
                *granule = le64(buf + i + 6);
                return pos + (long)i;
            }
        }
        pos += (long)n - PAGE_HDR + 1;            /* overlap so a header can't straddle */
    }
    return -1;
}

int64_t ogg_last_granule(ogg_reader_t *r)
{
    long saved = ftell(r->f);
    int64_t best = -1;
    for (long span = 64 * 1024; span <= 1024 * 1024 && best < 0; span *= 4) {
        long pos = r->file_size > span ? r->file_size - span : 0;
        int64_t g;
        long p;
        while ((p = find_page(r, pos, &g)) >= 0) {
            if (g >= 0) {
                best = g;
            }
            pos = p + 1;
        }
        if (r->file_size <= span) {
            break;                                /* already searched the whole file */
        }
    }
    fseek(r->f, saved, SEEK_SET);
    return best;
}

void ogg_seek_granule(ogg_reader_t *r, long data_start, int64_t target, int64_t *first_sample)
{
    long lo = data_start, hi = r->file_size, best = data_start;
    int64_t best_g = 0;
    /* bisection for the last page that ends before the target; decoding
     * restarts at the page after it, whose first sample is that granule */
    while (hi - lo > 4096) {
        long mid = lo + (hi - lo) / 2;
        int64_t g;
        long p = find_page(r, mid, &g);
        if (p < 0 || p >= hi) {
            hi = mid;
            continue;
        }
        if (g >= 0 && g < target) {
            best = p;
            best_g = g;
            lo = p + 1;
        } else {
            hi = mid;
        }
    }
    /* step from `best` (a page ending before target) to the page after it */
    long start = data_start;
    int64_t start_g = 0;
    if (best > data_start) {
        fseek(r->f, best, SEEK_SET);
        r->nseg = r->seg = r->seg_left = 0;
        if (read_page(r) == 1) {
            start = r->next_page_off;
            start_g = best_g;
        }
    }
    fseek(r->f, start, SEEK_SET);
    r->nseg = r->seg = r->seg_left = 0;
    r->in_packet = false;
    r->pkt_end = false;
    r->eof = false;
    /* a page that continues a packet from before: drop that fragment (it may
     * run on over further pages; packet_bytes follows it) */
    if (start > data_start && next_page(r) && (r->page_flags & 0x01)) {
        while (!r->pkt_end && !r->eof) {
            packet_bytes(r, NULL, 1 << 30);
        }
        r->pkt_end = false;
    }
    *first_sample = start_g;
}

bool opus_file_tags(FILE *f, id3_info_t *out)
{
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    ogg_reader_t r;
    opus_head_t h;
    ogg_init(&r, f, size);
    if (!opus_read_headers(&r, &h, out)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return out->title[0] || out->artist[0] || out->album[0];
}

/* ------------------------------------------------------- decode session */
#define PREROLL 3840                              /* 80 ms, RFC 7845 sec. 4.6 */

int opus_stream_open(opus_stream_t *s, FILE *f)
{
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    memset(s, 0, offsetof(opus_stream_t, pkt));
    ogg_init(&s->r, f, size);
    if (!opus_read_headers(&s->r, &s->head, NULL)) {
        return -1;
    }
    /* family 0 (mono/stereo) or family 1 with <= 2 channels is a single
     * plain Opus stream; real surround (multistream) is not supported */
    if (s->head.channels < 1 || s->head.channels > 2 || s->head.family > 1) {
        return -2;
    }
    s->data_start = s->r.next_page_off;
    s->last_gran = ogg_last_granule(&s->r);
    int err;
    s->dec = opus_decoder_create(OPUS_STREAM_RATE, 2, &err);
    if (!s->dec) {
        return -3;
    }
    opus_decoder_ctl(s->dec, OPUS_SET_GAIN(s->head.gain_q8));
    s->play_from = s->head.preskip;
    return 0;
}

int opus_stream_read(opus_stream_t *s, int16_t *pcm)
{
    for (;;) {
        int len = ogg_read_packet(&s->r, s->pkt, sizeof(s->pkt));
        if (len == -1) {
            return 0;                             /* end of stream */
        }
        if (len <= 0) {
            continue;                             /* oversized / empty packet */
        }
        int n = opus_decode(s->dec, s->pkt, len, pcm, OPUS_STREAM_MAX_FRAMES, 0);
        if (n < 0) {
            if (++s->bad > 50) {
                return -1;
            }
            continue;
        }
        s->bad = 0;
        /* Where this packet starts. A packet that ends a page ends exactly at
         * that page's granule, which re-anchors the count after a seek (the
         * fragment dropped there makes the estimate one packet short). The
         * last page is excluded: its granule is used for end trimming. */
        int64_t g0 = s->gran;
        bool page_end = s->r.seg >= s->r.nseg && s->r.seg_left == 0;
        if (page_end && s->r.page_granule >= 0 && s->r.page_granule != s->last_gran) {
            g0 = s->r.page_granule - n;
        }
        /* end trimming: the last page's granule says where the audio really ends */
        if (s->last_gran >= 0 && g0 + n > s->last_gran) {
            n = s->last_gran > g0 ? (int)(s->last_gran - g0) : 0;
        }
        s->gran = g0 + n;
        int start = 0;
        if (s->play_from > g0) {
            start = s->play_from - g0 < n ? (int)(s->play_from - g0) : n;
        }
        int frames = n - start;
        if (frames > 0) {
            if (start) {
                memmove(pcm, pcm + 2 * start, (size_t)frames * 2 * sizeof(int16_t));
            }
            return frames;
        }
        if (s->last_gran >= 0 && s->gran >= s->last_gran) {
            return 0;
        }
    }
}

uint64_t opus_stream_pos(const opus_stream_t *s)
{
    int64_t p = (s->gran > s->play_from ? s->gran : s->play_from) - s->head.preskip;
    return p > 0 ? (uint64_t)p : 0;
}

uint64_t opus_stream_total(const opus_stream_t *s)
{
    return s->last_gran > s->head.preskip ? (uint64_t)(s->last_gran - s->head.preskip) : 0;
}

void opus_stream_seek(opus_stream_t *s, uint64_t sample)
{
    int64_t target = (int64_t)sample + s->head.preskip;
    int64_t from = target > PREROLL ? target - PREROLL : 0;
    int64_t first = 0;
    ogg_seek_granule(&s->r, s->data_start, from, &first);
    opus_decoder_ctl(s->dec, OPUS_RESET_STATE);
    s->gran = first;
    s->play_from = target;
    s->bad = 0;
}

void opus_stream_close(opus_stream_t *s)
{
    if (s->dec) {
        opus_decoder_destroy(s->dec);
        s->dec = NULL;
    }
}
