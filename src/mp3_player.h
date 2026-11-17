#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP3_MUSIC_DIR "Music"       /* top-level folder on the card that is scanned */

/* Playback engine: scans the SD card's "Music" folder (MP3_MUSIC_DIR, and its
 * subfolders) for *.mp3 and *.opus, decodes MP3 with
 * libhelix (bundled in the esp32 Arduino core) and Ogg Opus with the vendored
 * libopus (src/opus, src/ogg_opus.c) in its own task, and streams to the
 * speaker. Thread-safe; the UI only sends commands and polls status. */

typedef enum { MP3_STOPPED, MP3_PLAYING, MP3_PAUSED } mp3_state_t;
typedef enum { MP3_REPEAT_OFF, MP3_REPEAT_ALL, MP3_REPEAT_ONE } mp3_repeat_t;
typedef enum { MP3_CODEC_MP3, MP3_CODEC_OPUS } mp3_codec_t;

typedef struct {
    mp3_state_t state;
    int track;              /* index into the playlist, -1 if none */
    uint32_t pos_ms;
    uint32_t dur_ms;        /* estimate; refined while playing (works for VBR) */
    uint32_t sample_rate;
    uint16_t kbps;
    uint8_t channels;
    uint8_t codec;          /* mp3_codec_t */
    uint32_t serial;        /* increments on every track change */
} mp3_status_t;

esp_err_t mp3_player_init(void);

/* Stops playback, remounts the card (picks up files the PC added over USB)
 * and rebuilds the playlist, sorted by path. Returns the track count, or -1
 * if the card is missing / not FAT32. Blocks briefly; call from the UI task. */
int mp3_player_scan(void);

int mp3_player_count(void);
/* ID3 title when the tag has one, otherwise the file name without folder/extension.
 * Tags are read by a background task after each scan, so this can change from
 * file name to tag title shortly after; watch mp3_player_meta_serial(). */
const char *mp3_player_title(int index);
const char *mp3_player_artist(int index);  /* ID3 artist, "" if absent / not read yet */
const char *mp3_player_album(int index);   /* ID3 album,  "" if absent / not read yet */
bool mp3_player_meta_ready(int index);     /* tag for this track has been read */
uint32_t mp3_player_meta_serial(void);     /* increments as tags are read */
const char *mp3_player_folder(int index);  /* subfolder inside Music, "" if directly in it */

void mp3_player_play(int index);
void mp3_player_toggle(void);              /* play/pause; starts track 0 when stopped */
void mp3_player_next(void);
void mp3_player_prev(void);                /* restarts the track if >3 s in */
void mp3_player_stop(void);
void mp3_player_seek(int permille);        /* 0..1000 of the track; ignored when stopped */

/* Shuffle: random order over the whole list, re-shuffled for each round with
 * repeat-all; the current track stays put when it is switched on.
 * Repeat: OFF stops after the last track, ALL wraps around, ONE replays the
 * current track (next/prev buttons still move on). Take effect immediately. */
void mp3_player_set_shuffle(bool on);
bool mp3_player_get_shuffle(void);
void mp3_player_set_repeat(mp3_repeat_t mode);
mp3_repeat_t mp3_player_get_repeat(void);

void mp3_player_get_status(mp3_status_t *out);

#ifdef __cplusplus
}
#endif
