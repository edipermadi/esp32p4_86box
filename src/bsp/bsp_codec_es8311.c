/*
 * ES8311 speaker codec (Waveshare 4B): I2C 0x18 on the shared bus, I2S slave.
 *
 * The register sequence is a DAC-only, slave-mode subset of Espressif's
 * esp_codec_dev ES8311 driver (Apache-2.0); the Arduino core does not ship it.
 */
#include "bsp_codec.h"
#if BSP_AUDIO_CODEC_ES8311
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "es8311";

#define ES8311_ADDR        0x18      /* CE pin low */
#define ES8311_I2C_HZ      100000

static i2c_master_dev_handle_t s_codec = NULL;

/* ---------------------------------------------------------------- ES8311 */
static esp_err_t es_wr(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_codec, buf, sizeof(buf), 100);
}

static esp_err_t es_rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_codec, &reg, 1, val, 1, 100);
}

/* Read-modify-write: reg = (reg & keep) | set */
static esp_err_t es_rmw(uint8_t reg, uint8_t keep, uint8_t set)
{
    uint8_t v = 0;
    ESP_RETURN_ON_ERROR(es_rd(reg, &v), TAG, "read 0x%02x failed", reg);
    return es_wr(reg, (v & keep) | set);
}

/* ES8311 clocking for MCLK = 256 * fs. Per Espressif's coefficient table, every
 * standard rate uses pre_div 1, x1, adc/dac_div 1, single speed, LRCK div 256,
 * BCLK div 4, adc_osr 0x10; only dac_osr differs (0x20 up to 16 kHz). */
static esp_err_t es8311_set_clock(uint32_t rate)
{
    esp_err_t r = ESP_OK;
    r |= es_rmw(0x02, 0x07, 0x00);                         /* pre_div 1, pre_multi x1 */
    r |= es_wr(0x05, 0x00);                                /* adc_div = dac_div = 1 */
    r |= es_rmw(0x03, 0x80, 0x10);                         /* single speed, adc_osr */
    r |= es_rmw(0x04, 0x80, rate <= 16000 ? 0x20 : 0x10);  /* dac_osr */
    r |= es_rmw(0x07, 0xC0, 0x00);                         /* LRCK div high */
    r |= es_wr(0x08, 0xFF);                                /* LRCK div low */
    r |= es_rmw(0x06, 0xE0, 4 - 1);                        /* BCLK div 4 */
    return r == ESP_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t es8311_init(uint32_t sample_rate)
{
    uint8_t id1 = 0, id2 = 0;
    ESP_RETURN_ON_ERROR(es_rd(0xFD, &id1), TAG, "ES8311 not responding at 0x%02x", ES8311_ADDR);
    es_rd(0xFE, &id2);
    ESP_LOGI(TAG, "ES8311 chip id %02x%02x", id1, id2);

    esp_err_t r = ESP_OK;
    /* open: base configuration */
    r |= es_wr(0x0D, 0xFA);
    r |= es_wr(0x44, 0x08);              /* I2C noise immunity (written twice on purpose) */
    r |= es_wr(0x44, 0x08);
    r |= es_wr(0x01, 0x30);
    r |= es_wr(0x02, 0x00);
    r |= es_wr(0x03, 0x10);
    r |= es_wr(0x16, 0x24);
    r |= es_wr(0x04, 0x10);
    r |= es_wr(0x05, 0x00);
    r |= es_wr(0x0B, 0x00);
    r |= es_wr(0x0C, 0x00);
    r |= es_wr(0x10, 0x1F);
    r |= es_wr(0x11, 0x7F);
    r |= es_wr(0x00, 0x80);              /* slave mode */
    r |= es_wr(0x01, 0x3F);              /* MCLK from MCLK pin, not inverted */
    r |= es_rmw(0x06, (uint8_t)~0x20, 0x00); /* SCLK not inverted */
    r |= es_wr(0x13, 0x10);
    r |= es_wr(0x1B, 0x0A);
    r |= es_wr(0x1C, 0x6A);
    r |= es_wr(0x44, 0x58);

    /* format: I2S, 16-bit */
    r |= es_rmw(0x09, 0xFC, 0x0C);
    r |= es_rmw(0x0A, 0xFC, 0x0C);

    /* sample-rate clocking */
    r |= es8311_set_clock(sample_rate);

    /* start (DAC path) */
    r |= es_wr(0x00, 0x80);
    r |= es_wr(0x01, 0x3F);
    r |= es_rmw(0x09, 0xBF, 0x00);       /* SDP in: unmute */
    r |= es_wr(0x17, 0xBF);
    r |= es_wr(0x0E, 0x02);
    r |= es_wr(0x12, 0x00);              /* power up DAC */
    r |= es_wr(0x14, 0x1A);
    r |= es_wr(0x0D, 0x01);
    r |= es_wr(0x15, 0x40);
    r |= es_wr(0x37, 0x08);
    r |= es_wr(0x45, 0x00);
    r |= es_rmw(0x31, 0x9F, 0x00);       /* DAC soft-unmute */

    ESP_RETURN_ON_FALSE(r == ESP_OK, ESP_FAIL, TAG, "ES8311 register setup failed");
    return ESP_OK;
}

void bsp_codec_set_volume(int percent)
{
    if (!s_codec) {
        return;
    }
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* Reg 0x32: 0x00 = -95.5 dB, 0xBF = 0 dB, 0.5 dB/step. Map 1..100 % to -47.5..0 dB. */
    uint8_t reg = percent == 0 ? 0x00 : (uint8_t)(0x5F + (0xBF - 0x5F) * percent / 100);
    es_wr(0x32, reg);
}

esp_err_t bsp_codec_open(i2c_master_bus_handle_t bus, uint32_t sample_rate)
{
    if (!s_codec) {
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ES8311_ADDR,
            .scl_speed_hz = ES8311_I2C_HZ,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev, &s_codec), TAG, "add ES8311 failed");
    }
    return es8311_init(sample_rate);
}

esp_err_t bsp_codec_set_rate(uint32_t sample_rate)
{
    return es8311_set_clock(sample_rate);
}

void bsp_codec_mute(bool mute)
{
    if (s_codec) {
        es_rmw(0x31, 0x9F, mute ? 0x60 : 0x00);     /* DAC soft-mute */
    }
}

#endif /* BSP_AUDIO_CODEC_ES8311 */
