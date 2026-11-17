#include "settings.h"
#include "mp3_player.h"
#include "esp_log.h"
#include "nvs.h"
#include <lvgl.h>
#include <string.h>

static const char *TAG = "settings";
#define NS "player"

void settings_load(player_settings_t *s)
{
    s->volume = 70;
    s->shuffle = false;
    s->repeat = MP3_REPEAT_OFF;
    s->show_list = true;
    s->brightness = 100;
    s->waterfall = false;
    s->timeout_min = 5;
    s->clock24 = true;
    s->usb_drive = true;
    s->gallery_random = false;
    s->rotation = 0;
    strcpy(s->tz, "UTC");

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;                                    /* first boot: nothing stored yet */
    }
    uint8_t v;
    if (nvs_get_u8(h, "volume", &v) == ESP_OK && v <= 100) {
        s->volume = v;
    }
    if (nvs_get_u8(h, "shuffle", &v) == ESP_OK && v <= 1) {
        s->shuffle = v;
    }
    if (nvs_get_u8(h, "repeat", &v) == ESP_OK && v <= MP3_REPEAT_ONE) {
        s->repeat = v;
    }
    if (nvs_get_u8(h, "list", &v) == ESP_OK && v <= 1) {
        s->show_list = v;
    }
    if (nvs_get_u8(h, "bright", &v) == ESP_OK && v >= 10 && v <= 100) {
        s->brightness = v;
    }
    if (nvs_get_u8(h, "waterfall", &v) == ESP_OK && v <= 1) {
        s->waterfall = v;
    }
    if (nvs_get_u8(h, "timeout", &v) == ESP_OK && v <= 60) {
        s->timeout_min = v;
    }
    if (nvs_get_u8(h, "clock24", &v) == ESP_OK && v <= 1) {
        s->clock24 = v;
    }
    if (nvs_get_u8(h, "usbdrive", &v) == ESP_OK && v <= 1) {
        s->usb_drive = v;
    }
    if (nvs_get_u8(h, "galrand", &v) == ESP_OK && v <= 1) {
        s->gallery_random = v;
    }
    if (nvs_get_u8(h, "rot", &v) == ESP_OK && v <= 3) {
        s->rotation = v;
    }
    size_t n = sizeof(s->tz);
    if (nvs_get_str(h, "tz", s->tz, &n) != ESP_OK) {
        strcpy(s->tz, "UTC");
    }
    nvs_close(h);
    ESP_LOGI(TAG, "loaded: volume %u, shuffle %d, repeat %u, list %d",
             s->volume, s->shuffle, s->repeat, s->show_list);
}

static bool put(nvs_handle_t h, const char *key, uint8_t val)
{
    uint8_t old;
    if (nvs_get_u8(h, key, &old) == ESP_OK && old == val) {
        return false;                              /* unchanged: no flash write */
    }
    esp_err_t err = nvs_set_u8(h, key, val);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set %s failed: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool put_str(nvs_handle_t h, const char *key, const char *val)
{
    char old[64];
    size_t n = sizeof(old);
    if (nvs_get_str(h, key, old, &n) == ESP_OK && strcmp(old, val) == 0) {
        return false;
    }
    return nvs_set_str(h, key, val) == ESP_OK;
}

void settings_save(const player_settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        return;
    }
    bool dirty = put(h, "volume", s->volume);
    dirty |= put(h, "shuffle", s->shuffle);
    dirty |= put(h, "repeat", s->repeat);
    dirty |= put(h, "list", s->show_list);
    dirty |= put(h, "bright", s->brightness);
    dirty |= put(h, "waterfall", s->waterfall);
    dirty |= put(h, "timeout", s->timeout_min);
    dirty |= put(h, "clock24", s->clock24);
    dirty |= put(h, "usbdrive", s->usb_drive);
    dirty |= put(h, "galrand", s->gallery_random);
    dirty |= put(h, "rot", s->rotation);
    dirty |= put_str(h, "tz", s->tz);
    if (dirty) {
        err = nvs_commit(h);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "commit failed: %s", esp_err_to_name(err));
        }
    }
    nvs_close(h);
}

/* ---- shared copy + delayed save ---- */
static player_settings_t s_cur;
static bool s_loaded;
static lv_timer_t *s_save_timer;

player_settings_t *settings_get(void)
{
    if (!s_loaded) {
        settings_load(&s_cur);
        s_loaded = true;
    }
    return &s_cur;
}

static void save_cb(lv_timer_t *t)
{
    settings_save(&s_cur);
    lv_timer_pause(t);
}

void settings_changed(void)
{
    if (!s_save_timer) {
        s_save_timer = lv_timer_create(save_cb, 1500, NULL);
    }
    lv_timer_reset(s_save_timer);
    lv_timer_resume(s_save_timer);
}
