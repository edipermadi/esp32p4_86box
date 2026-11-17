/*
 * Board support API shared by the supported ESP32-P4 boards (pick one in
 * bsp_config.h):
 *
 *  - Waveshare ESP32-P4-WIFI6-Touch-LCD-4B: 4" 720x720 MIPI-DSI (ST7703 or
 *    JD9365), GT911 touch, ES8311 speaker. Pins/timings from Waveshare's BSP
 *    (github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4B, waveshare_boards).
 *  - M5Stack Tab5: 5" 720x1280 MIPI-DSI (ILI9881C+GT911, or ST7123/ST7121 with
 *    built-in touch), ES8388 speaker, two PI4IOE5V6408 I/O expanders for the
 *    power/reset lines. Pins/timings from M5Stack's M5Tab5-UserDemo BSP.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bsp_config.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "driver/i2c_master.h"
#include "sd_protocol_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(BSP_BOARD_WAVESHARE_4B)
/* ============================================= Waveshare ESP32-P4 LCD-4B */
#define BSP_BOARD_NAME           "Waveshare ESP32-P4 4B"

#define BSP_LCD_H_RES            720
#define BSP_LCD_V_RES            720
#define BSP_LCD_BITS_PER_PIXEL   16      /* RGB565 */
#define BSP_LCD_DSI_LANES        2
#define BSP_LCD_DSI_LANE_MBPS    480
#define BSP_LCD_RST_GPIO         27      /* active low */
#define BSP_LCD_BACKLIGHT_GPIO   26      /* LEDC PWM */
#define BSP_LCD_BACKLIGHT_INVERT 1       /* active low */
#define BSP_MIPI_DPHY_LDO_CHAN   3       /* LDO_VO3 -> VDD_MIPI_DPHY */
#define BSP_MIPI_DPHY_LDO_MV     2500

#define BSP_I2C_PORT             1       /* Arduino Wire uses port 0 */
#define BSP_I2C_SDA_GPIO         7
#define BSP_I2C_SCL_GPIO         8
#define BSP_TOUCH_I2C_HZ         400000

#define BSP_AUDIO_CODEC_ES8311   1
#define BSP_I2S_MCLK_GPIO        13
#define BSP_I2S_BCLK_GPIO        12
#define BSP_I2S_WS_GPIO          10
#define BSP_I2S_DOUT_GPIO        9       /* SoC -> ES8311 DSDIN */
#define BSP_I2S_DIN_GPIO         11      /* ES7210 mic ADC -> SoC (unused here) */
#define BSP_AUDIO_PA_GPIO        53      /* NS4150 amp enable, active high */

/* ESP-Hosted (C6 on SDMMC slot 1): the core's defaults (CLK18 CMD19 D0-3 14-17, RST54) match */

#elif defined(BSP_BOARD_M5STACK_TAB5)
/* ======================================================== M5Stack Tab5 */
#define BSP_BOARD_NAME           "M5Stack Tab5"

#define BSP_LCD_H_RES            720
#define BSP_LCD_V_RES            1280
#define BSP_LCD_BITS_PER_PIXEL   16      /* RGB565 */
#define BSP_LCD_DSI_LANES        2
#define BSP_LCD_BACKLIGHT_GPIO   22      /* LEDC PWM, active high */
#define BSP_LCD_BACKLIGHT_INVERT 0
#define BSP_MIPI_DPHY_LDO_CHAN   3
#define BSP_MIPI_DPHY_LDO_MV     2500
#define BSP_TOUCH_INT_GPIO       23      /* held high while the touch leaves reset -> GT911 at 0x14 */

#define BSP_I2C_PORT             1       /* "SYS" I2C: expanders, touch, codecs */
#define BSP_I2C_SDA_GPIO         31
#define BSP_I2C_SCL_GPIO         32
#define BSP_TOUCH_I2C_HZ         400000

#define BSP_AUDIO_CODEC_ES8388   1
#define BSP_I2S_MCLK_GPIO        30
#define BSP_I2S_BCLK_GPIO        27
#define BSP_I2S_WS_GPIO          29
#define BSP_I2S_DOUT_GPIO        26      /* SoC -> ES8388 DSDIN */
#define BSP_I2S_DIN_GPIO         28      /* ES7210 mic ADC -> SoC (unused here) */
/* speaker amp enable is I/O expander 1, P1 */

/* ESP-Hosted (C6 on SDMMC slot 1): not the core's default pins */
#define BSP_HOSTED_SDIO_CLK      12
#define BSP_HOSTED_SDIO_CMD      13
#define BSP_HOSTED_SDIO_D0       11
#define BSP_HOSTED_SDIO_D1       10
#define BSP_HOSTED_SDIO_D2       9
#define BSP_HOSTED_SDIO_D3       8
#define BSP_HOSTED_RESET         15

#endif

/* --------------------------------------------------------------- microSD */
/* Both boards: SDMMC slot 0 (IO_MUX): CLK 43, CMD 44, D0..D3 = 39..42, no card-detect. */
#define BSP_SD_LDO_CHAN          4       /* LDO_VO4 -> SD IO rail */
#define BSP_SD_LDO_MV            3300

typedef enum {
    BSP_PANEL_UNKNOWN = 0,
    BSP_PANEL_ST7703,       /* 4B */
    BSP_PANEL_JD9365,       /* 4B */
    BSP_PANEL_ILI9881C,     /* Tab5, GT911 touch */
    BSP_PANEL_ST7123,       /* Tab5, touch in the panel controller */
    BSP_PANEL_ST7121,       /* Tab5, touch in the panel controller */
} bsp_panel_kind_t;

/* Shared I2C bus (touch, codecs, camera; I/O expanders on the Tab5). Created on
 * first call; on the Tab5 this also runs the expander power-up sequence. */
esp_err_t bsp_i2c_init(i2c_master_bus_handle_t *ret_bus);

/* Speaker amplifier on/off (GPIO on the 4B, I/O expander on the Tab5). */
void bsp_audio_amp_set(bool on);

/* Powers the D-PHY, brings up the DSI link, detects which panel is fitted and
 * initialises it. Backlight is left off. */
esp_err_t bsp_display_new(esp_lcd_panel_handle_t *ret_panel);
bsp_panel_kind_t bsp_display_panel_kind(void);
/* Controller name of the detected panel, e.g. "ST7703". */
const char *bsp_display_panel_name(void);

/* Backlight brightness 0..100 % (implemented with the Arduino LEDC API). */
void bsp_display_brightness_set(int percent);

/* Touch, polled: GT911 (both boards) or the ST7123/ST7121 built-in controller (Tab5). */
esp_err_t bsp_touch_init(void);
/* Returns true while touched; fills the first contact point. */
bool bsp_touch_read(uint16_t *x, uint16_t *y);

/* All of the above + LVGL display/input-device registration.
 * After this returns, create your UI and call lv_timer_handler() from loop(). */
esp_err_t bsp_lvgl_start(void);

/* Speaker: I2S0 (TX only, 16-bit stereo) -> ES8311 (4B) / ES8388 (Tab5) -> amp. Supported rates:
 * 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000.
 * Turns the amp on when done. */
esp_err_t bsp_audio_init(uint32_t sample_rate);
/* Re-clocks I2S + codec; call only while holding the audio (see acquire). */
esp_err_t bsp_audio_set_sample_rate(uint32_t sample_rate);
/* Exclusive use of the speaker for one audio producer (the MP3 player takes it per track). */
bool bsp_audio_acquire(uint32_t timeout_ms);
void bsp_audio_release(void);
/* Blocking write of interleaved L/R 16-bit frames. */
esp_err_t bsp_audio_write(const int16_t *stereo_frames, size_t frames, uint32_t timeout_ms);
/* 0..100 % (0 = mute, 100 = 0 dB codec gain). */
void bsp_audio_set_volume(int percent);

/* Powers the TF slot and initialises the card for raw sector access (no
 * filesystem mount). Fails with an error if no card is inserted. */
esp_err_t bsp_sdcard_init(sdmmc_card_t **ret_card);

/* Sector I/O. Serialised with a lock: use these instead of sdmmc_*_sectors()
 * so the USB drive and the firmware's FAT reads never interleave on the bus. */
esp_err_t bsp_sdcard_read(void *buf, uint32_t sector, uint32_t count);
esp_err_t bsp_sdcard_write(const void *buf, uint32_t sector, uint32_t count);

/* Mounts the card's FAT32 filesystem at BSP_SD_MOUNT_POINT for the firmware's
 * own (read-only by convention) use, alongside the USB drive. Calling it again
 * remounts, dropping FatFs caches so files the PC added become visible. */
#define BSP_SD_MOUNT_POINT       "/sd"
esp_err_t bsp_sdcard_mount(void);

/* Held by bsp_sdcard_mount() while it (re)mounts. A task that reads files
 * outside the player (e.g. the slideshow loader) holds it from opendir/fopen
 * to closedir/fclose, so a remount never pulls the filesystem out from under
 * an open file. Recursive; no-ops before the card was first initialised. */
void bsp_sdcard_fs_lock(void);
void bsp_sdcard_fs_unlock(void);

#ifdef __cplusplus
}
#endif
