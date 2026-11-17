/*
 * Minimal ID3 reader: TIT2/TPE1/TALB (v2.3/2.4), TT2/TP1/TAL (v2.2), ID3v1.
 * Text is converted to UTF-8 from ISO-8859-1, UTF-16 (with BOM / BE) or UTF-8.
 */
#include "id3.h"
#include <stdint.h>
#include <string.h>

#define FRAME_READ_MAX 512      /* text frames longer than this are truncated */

static uint32_t syncsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Appends one code point as UTF-8; returns false when dst is full. */
static bool put_utf8(char *dst, size_t cap, size_t *len, uint32_t cp)
{
    uint8_t b[4];
    size_t n;
    if (cp < 0x20 && cp != '\t') {
        return true;                                  /* drop control chars */
    }
    if (cp < 0x80) {
        b[0] = (uint8_t)cp; n = 1;
    } else if (cp < 0x800) {
        b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3F); n = 2;
    } else if (cp < 0x10000) {
        b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3F); b[2] = 0x80 | (cp & 0x3F); n = 3;
    } else {
        b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3F);
        b[2] = 0x80 | ((cp >> 6) & 0x3F); b[3] = 0x80 | (cp & 0x3F); n = 4;
    }
    if (*len + n >= cap) {
        return false;
    }
    memcpy(dst + *len, b, n);
    *len += n;
    return true;
}

static void trim(char *s, size_t len)
{
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\0')) {
        len--;
    }
    s[len] = '\0';
}

static void latin1_to_utf8(const uint8_t *src, size_t n, char *dst, size_t cap)
{
    size_t len = 0;
    for (size_t i = 0; i < n && src[i]; i++) {
        if (!put_utf8(dst, cap, &len, src[i])) {
            break;
        }
    }
    trim(dst, len);
}

/* data[0] is the ID3v2 text-encoding byte. Only the first string is used. */
static void text_to_utf8(const uint8_t *data, size_t n, char *dst, size_t cap)
{
    size_t len = 0;
    if (n < 1) {
        dst[0] = '\0';
        return;
    }
    uint8_t enc = data[0];
    const uint8_t *p = data + 1;
    n -= 1;

    if (enc == 0) {                                   /* ISO-8859-1 */
        latin1_to_utf8(p, n, dst, cap);
        return;
    }
    if (enc == 3) {                                   /* UTF-8 */
        size_t i = 0;
        while (i < n && p[i] && i + 1 < cap) {
            dst[i] = (char)p[i];
            i++;
        }
        if (i < n && p[i]) {                          /* truncated: don't leave half a sequence */
            while (i > 0 && ((uint8_t)dst[i - 1] & 0xC0) == 0x80) {
                i--;
            }
            if (i > 0 && ((uint8_t)dst[i - 1] & 0x80)) {
                i--;
            }
        }
        trim(dst, i);
        return;
    }

    /* UTF-16: enc 1 = with BOM, enc 2 = big endian */
    bool be = (enc == 2);
    if (enc == 1 && n >= 2) {
        if (p[0] == 0xFF && p[1] == 0xFE) { be = false; p += 2; n -= 2; }
        else if (p[0] == 0xFE && p[1] == 0xFF) { be = true; p += 2; n -= 2; }
    }
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = be ? ((uint32_t)p[i] << 8 | p[i + 1]) : ((uint32_t)p[i + 1] << 8 | p[i]);
        if (u == 0) {
            break;
        }
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) { /* surrogate pair */
            uint32_t lo = be ? ((uint32_t)p[i + 2] << 8 | p[i + 3]) : ((uint32_t)p[i + 3] << 8 | p[i + 2]);
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (!put_utf8(dst, cap, &len, u)) {
            break;
        }
    }
    trim(dst, len);
}

static void read_v2(FILE *f, id3_info_t *o)
{
    uint8_t h[10];
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) != 0 || h[3] < 2 || h[3] > 4) {
        return;
    }
    const int ver = h[3];
    const long end = 10 + (long)syncsafe32(h + 6);
    long pos = 10;

    if ((h[5] & 0x40) && ver >= 3) {                  /* extended header */
        uint8_t e[4];
        if (fread(e, 1, 4, f) != 4) {
            return;
        }
        pos += (ver == 4) ? (long)syncsafe32(e) : (long)be32(e) + 4;
        fseek(f, pos, SEEK_SET);
    }

    const int id_len = (ver == 2) ? 3 : 4;
    const int hdr_len = (ver == 2) ? 6 : 10;
    static const char *ids22[] = { "TT2", "TP1", "TAL" };
    static const char *ids23[] = { "TIT2", "TPE1", "TALB" };
    char *dsts[] = { o->title, o->artist, o->album };

    while (pos + hdr_len <= end) {
        uint8_t fh[10];
        if (fread(fh, 1, hdr_len, f) != (size_t)hdr_len || fh[0] == 0) {
            break;                                    /* EOF or padding */
        }
        uint32_t size;
        uint16_t flags = 0;
        if (ver == 2) {
            size = ((uint32_t)fh[3] << 16) | ((uint32_t)fh[4] << 8) | fh[5];
        } else if (ver == 3) {
            size = be32(fh + 4);
            flags = (uint16_t)(fh[8] << 8 | fh[9]);
        } else {
            size = syncsafe32(fh + 4);
            flags = (uint16_t)(fh[8] << 8 | fh[9]);
        }
        pos += hdr_len;
        if (size == 0 || pos + (long)size > end) {
            break;
        }

        for (int k = 0; k < 3; k++) {
            if (dsts[k][0] || memcmp(fh, ver == 2 ? ids22[k] : ids23[k], id_len) != 0) {
                continue;
            }
            uint32_t pre = 0;
            bool skip = false;
            if (ver == 3 && (flags & 0x00C0)) {
                skip = true;                          /* compressed / encrypted */
            }
            if (ver == 4) {
                skip = (flags & 0x000C) != 0;         /* compressed / encrypted */
                pre = (flags & 0x0001) ? 4 : 0;       /* data length indicator */
            }
            if (!skip && size > pre) {
                uint8_t buf[FRAME_READ_MAX];
                size_t n = size - pre;
                if (n > sizeof(buf)) {
                    n = sizeof(buf);
                }
                fseek(f, pos + pre, SEEK_SET);
                if (fread(buf, 1, n, f) == n) {
                    text_to_utf8(buf, n, dsts[k], ID3_FIELD_MAX);
                }
            }
            break;
        }
        if (o->title[0] && o->artist[0] && o->album[0]) {
            break;
        }
        pos += size;                                  /* skips album art etc. */
        fseek(f, pos, SEEK_SET);
    }
}

static void read_v1(FILE *f, id3_info_t *o)
{
    uint8_t t[128];
    if (fseek(f, -128, SEEK_END) != 0 || fread(t, 1, 128, f) != 128 || memcmp(t, "TAG", 3) != 0) {
        return;
    }
    if (!o->title[0])  latin1_to_utf8(t + 3, 30, o->title, ID3_FIELD_MAX);
    if (!o->artist[0]) latin1_to_utf8(t + 33, 30, o->artist, ID3_FIELD_MAX);
    if (!o->album[0])  latin1_to_utf8(t + 63, 30, o->album, ID3_FIELD_MAX);
}

bool id3_read(FILE *f, id3_info_t *o)
{
    memset(o, 0, sizeof(*o));
    fseek(f, 0, SEEK_SET);
    read_v2(f, o);
    if (!o->title[0] || !o->artist[0] || !o->album[0]) {
        read_v1(f, o);
    }
    return o->title[0] || o->artist[0] || o->album[0];
}
