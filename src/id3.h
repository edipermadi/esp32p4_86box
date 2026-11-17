#pragma once
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ID3_FIELD_MAX 128

typedef struct {
    char title[ID3_FIELD_MAX];    /* UTF-8, "" if absent */
    char artist[ID3_FIELD_MAX];
    char album[ID3_FIELD_MAX];
} id3_info_t;

/* Reads title/artist/album from an ID3v2.2/2.3/2.4 tag at the start of the
 * file, filling any gaps from an ID3v1 tag at the end. Skips large frames
 * (album art) without reading them. Returns true if any field was found. */
bool id3_read(FILE *f, id3_info_t *out);

#ifdef __cplusplus
}
#endif
