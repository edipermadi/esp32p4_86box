#pragma once
/*
 * Ogg container + Opus headers (RFC 3533, RFC 7845), without libogg.
 * Reads one logical stream (the first one in the file); pages of other
 * streams are skipped. Packets are streamed, so huge ones (cover art in
 * OpusTags) never need to fit in memory.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "id3.h"
#include "opus/include/opus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    FILE *f;
    long file_size;
    uint32_t serial;
    bool have_serial;
    uint8_t lacing[255];
    int nseg, seg;              /* segments in the current page / next one to start */
    int seg_left, last_lace;    /* bytes left in the current segment, its lacing value */
    bool in_packet, pkt_end;
    bool eof;
    long next_page_off;         /* file offset right after the current page */
    int64_t page_granule;
    uint8_t page_flags;         /* 0x01 = page starts with a continued packet */
} ogg_reader_t;

typedef struct {
    int channels;
    int preskip;                /* samples at 48 kHz to drop at the start */
    int gain_q8;                /* output gain, dB in Q7.8 */
    int family;                 /* channel mapping family */
} opus_head_t;

void ogg_init(ogg_reader_t *r, FILE *f, long file_size);

/* Reads the whole next packet into buf. Returns its length, -1 at end of
 * stream, or -2 if it was larger than cap (the packet is skipped). */
int ogg_read_packet(ogg_reader_t *r, uint8_t *buf, int cap);

/* First two packets of an Ogg Opus stream. tags may be NULL to just skip them. */
bool opus_read_headers(ogg_reader_t *r, opus_head_t *head, id3_info_t *tags);

/* Granule position of the last page of the stream (= end sample incl. pre-skip), or -1. */
int64_t ogg_last_granule(ogg_reader_t *r);

/* Positions the reader on a page boundary at or before the page that holds
 * `target` (48 kHz granule units), searching between data_start and EOF.
 * *first_sample receives the granule of the first sample decoded from there. */
void ogg_seek_granule(ogg_reader_t *r, long data_start, int64_t target, int64_t *first_sample);

/* ---- decode session: what the player uses ---------------------------- */
#define OPUS_STREAM_RATE        48000
#define OPUS_STREAM_MAX_FRAMES  5760     /* 120 ms at 48 kHz: pcm buffer size for read() */

typedef struct {
    ogg_reader_t r;
    opus_head_t head;
    OpusDecoder *dec;
    long data_start;            /* first audio page */
    int64_t last_gran;          /* end granule, -1 if unknown */
    int64_t gran;               /* granule after the last decoded packet */
    int64_t play_from;          /* granule of the first sample to output (pre-skip / seek target) */
    int bad;                    /* consecutive undecodable packets */
    uint8_t pkt[8192];
} opus_stream_t;

/* Parses the headers and creates a stereo 48 kHz decoder (mono streams are
 * upmixed by libopus). 0 on success, <0 if this is not a playable Ogg Opus file. */
int opus_stream_open(opus_stream_t *s, FILE *f);
/* Decodes up to the next packet into pcm (interleaved stereo, room for
 * OPUS_STREAM_MAX_FRAMES frames). Returns frames, 0 at the end, <0 if the
 * stream is broken. */
int opus_stream_read(opus_stream_t *s, int16_t *pcm);
/* Output position / length in samples at 48 kHz (pre-skip excluded). */
uint64_t opus_stream_pos(const opus_stream_t *s);
uint64_t opus_stream_total(const opus_stream_t *s);
/* Seeks to an output sample (sample-accurate: decodes 80 ms of pre-roll). */
void opus_stream_seek(opus_stream_t *s, uint64_t sample);
void opus_stream_close(opus_stream_t *s);

/* Convenience for the tag scanner: title/artist/album of an .opus file. */
bool opus_file_tags(FILE *f, id3_info_t *out);

#ifdef __cplusplus
}
#endif
