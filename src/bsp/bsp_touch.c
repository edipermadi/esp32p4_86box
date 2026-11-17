/*
 * Capacitive touch, polled over the shared I2C bus.
 *
 * GT911 (Waveshare 4B; Tab5 with the ILI9881C panel). On the 4B INT is not
 * routed and the reset line is not driven, so the controller can come up at
 * either address; on the Tab5 the power-up sequence selects 0x14, and the INT
 * line then needs the GT911 "wake" pulse (low 58 ms, high, then input with
 * pull-up, as M5GFX does) before the chip reports touches.
 *
 * ST7123 / ST7121 (later Tab5 panels): touch is part of the display controller,
 * at 0x55 with 16-bit big-endian register addresses. Register map as used by
 * M5Stack's drivers: 0x0009 max touches, 0x0010 status (bit 3 = coordinates
 * present), 0x0014.. 7-byte reports {valid|x_hi, x_lo, y_hi, y_lo, area, ...}.
 */
#include "bsp_board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_touch";

#define GT911_ADDR_PRIMARY   0x5D
#define GT911_ADDR_BACKUP    0x14
#define GT911_REG_PRODUCT_ID 0x8140
#define GT911_REG_STATUS     0x814E
#define GT911_REG_POINT1     0x814F

#define ST7123_ADDR          0x55
#define ST7123_REG_FW_VER    0x0000
#define ST7123_REG_MAX_TOUCH 0x0009
#define ST7123_REG_STATUS    0x0010
#define ST7123_REG_REPORT    0x0014
#define ST7123_REPORT_LEN    7
#define ST7123_MAX_POINTS    10

typedef enum { TP_NONE, TP_GT911, TP_ST7123 } tp_kind_t;

static i2c_master_dev_handle_t s_tp = NULL;
static tp_kind_t s_kind = TP_NONE;
static uint8_t s_st_points = 1;

/* live counters for bsp_touch_diag() */
static uint32_t s_polls, s_errors, s_presses, s_retries;
static uint8_t s_last_status;
static uint16_t s_last_x, s_last_y;

static esp_err_t reg16_read(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t r[2] = {reg >> 8, reg & 0xFF};
    return i2c_master_transmit_receive(s_tp, r, 2, buf, len, 50);
}

static esp_err_t gt911_write_u8(uint16_t reg, uint8_t val)
{
    uint8_t w[3] = {reg >> 8, reg & 0xFF, val};
    return i2c_master_transmit(s_tp, w, 3, 50);
}

static esp_err_t add_device(i2c_master_bus_handle_t bus, uint16_t addr)
{
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = BSP_TOUCH_I2C_HZ,
    };
    return i2c_master_bus_add_device(bus, &dev, &s_tp);
}

/* One pass over the known addresses; fills s_tp/s_kind on success. */
static bool probe_once(i2c_master_bus_handle_t bus)
{
    uint16_t addr = 0;
    if (i2c_master_probe(bus, GT911_ADDR_PRIMARY, 50) == ESP_OK) {
        addr = GT911_ADDR_PRIMARY;
    } else if (i2c_master_probe(bus, GT911_ADDR_BACKUP, 50) == ESP_OK) {
        addr = GT911_ADDR_BACKUP;
    }
    if (addr) {
        if (add_device(bus, addr) != ESP_OK) {
            return false;
        }
#ifdef BSP_TOUCH_INT_GPIO
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << BSP_TOUCH_INT_GPIO,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io);
        gpio_set_level(BSP_TOUCH_INT_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(58));
        gpio_set_level(BSP_TOUCH_INT_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(2));
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&io);
#endif
        gt911_write_u8(GT911_REG_STATUS, 0);   /* clear any stale report */
        uint8_t id[4] = {0};
        reg16_read(GT911_REG_PRODUCT_ID, id, 4);
        ESP_LOGI(TAG, "GT911 at 0x%02X, product id \"%.4s\"", addr, (char *)id);
        bsp_diag_note(BSP_DIAG_TOUCH, "GT911 at 0x%02X, product id \"%.4s\"", addr, (char *)id);
        s_kind = TP_GT911;
        return true;
    }
#if defined(BSP_BOARD_M5STACK_TAB5)
    if (i2c_master_probe(bus, ST7123_ADDR, 50) == ESP_OK) {
        if (add_device(bus, ST7123_ADDR) != ESP_OK) {
            return false;
        }
        uint8_t fw = 0, maxp = 0;
        reg16_read(ST7123_REG_FW_VER, &fw, 1);
        reg16_read(ST7123_REG_MAX_TOUCH, &maxp, 1);
        s_st_points = (maxp >= 1 && maxp <= ST7123_MAX_POINTS) ? maxp : ST7123_MAX_POINTS;
        ESP_LOGI(TAG, "ST712x touch at 0x55, fw %u, %u points", fw, s_st_points);
        bsp_diag_note(BSP_DIAG_TOUCH, "ST712x at 0x55, fw %u, max points reg %u", fw, maxp);
        s_kind = TP_ST7123;
        return true;
    }
#endif
    return false;
}

esp_err_t bsp_touch_init(void)
{
    if (s_kind != TP_NONE) {
        return ESP_OK;
    }
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(bsp_i2c_init(&bus), TAG, "I2C");

    /* the Tab5's touch controller can take a moment after power-up */
    for (int tries = 0; tries < 30; tries++) {
        if (tries) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (probe_once(bus)) {
            return ESP_OK;
        }
    }
    ESP_LOGE(TAG, "no touch controller found");
    bsp_diag_note(BSP_DIAG_TOUCH, "none found at boot (retrying every 1 s)");
    return ESP_ERR_NOT_FOUND;
}

static bool clamp_store(uint16_t px, uint16_t py, uint16_t *lx, uint16_t *ly)
{
    *lx = px < BSP_LCD_H_RES ? px : BSP_LCD_H_RES - 1;
    *ly = py < BSP_LCD_V_RES ? py : BSP_LCD_V_RES - 1;
    return true;
}

static bool gt911_read(uint16_t *x, uint16_t *y)
{
    static bool last_pressed = false;
    static uint16_t last_x, last_y;

    uint8_t status = 0;
    if (reg16_read(GT911_REG_STATUS, &status, 1) != ESP_OK) {
        s_errors++;
        return false;
    }
    s_last_status = status;
    if (status & 0x80) {                 /* buffer ready: new data */
        uint8_t n = status & 0x0F;
        if (n > 0 && n <= 5) {
            uint8_t p[8];
            if (reg16_read(GT911_REG_POINT1, p, sizeof(p)) == ESP_OK) {
                last_pressed = clamp_store(p[1] | (p[2] << 8), p[3] | (p[4] << 8), &last_x, &last_y);
            }
        } else {
            last_pressed = false;
        }
        gt911_write_u8(GT911_REG_STATUS, 0);   /* ack */
    }
    if (last_pressed) {
        *x = last_x;
        *y = last_y;
    }
    return last_pressed;
}

static bool st7123_read(uint16_t *x, uint16_t *y)
{
    uint8_t status = 0;
    if (reg16_read(ST7123_REG_STATUS, &status, 1) != ESP_OK) {
        s_errors++;
        return false;
    }
    s_last_status = status;
    if (!(status & 0x08)) {
        return false;
    }
    uint8_t rep[ST7123_MAX_POINTS * ST7123_REPORT_LEN];
    if (reg16_read(ST7123_REG_REPORT, rep, (size_t)s_st_points * ST7123_REPORT_LEN) != ESP_OK) {
        s_errors++;
        return false;
    }
    for (int i = 0; i < s_st_points; i++) {
        const uint8_t *r = &rep[i * ST7123_REPORT_LEN];
        if (r[0] & 0x80) {               /* valid */
            return clamp_store(((r[0] & 0x3F) << 8) | r[1], (r[2] << 8) | r[3], x, y);
        }
    }
    return false;
}

bool bsp_touch_read(uint16_t *x, uint16_t *y)
{
    bool pressed = false;
    switch (s_kind) {
    case TP_GT911:  pressed = gt911_read(x, y); break;
    case TP_ST7123: pressed = st7123_read(x, y); break;
    default: {
        /* not found at boot: look again once a second (it may come up late) */
        static uint32_t next_ms;
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if ((int32_t)(now - next_ms) >= 0) {
            next_ms = now + 1000;
            s_retries++;
            i2c_master_bus_handle_t bus = NULL;
            if (bsp_i2c_init(&bus) == ESP_OK && probe_once(bus)) {
                bsp_diag_note(BSP_DIAG_TOUCH, "found late (retry %lu)", (unsigned long)s_retries);
            }
        }
        return false;
    }
    }
    static bool was_pressed;
    s_polls++;
    if (pressed) {
        if (!was_pressed) {
            s_presses++;
        }
        s_last_x = *x;
        s_last_y = *y;
    }
    was_pressed = pressed;
    return pressed;
}

size_t bsp_touch_diag(char *buf, size_t len)
{
    int k = snprintf(buf, len,
                     "[diag] touch live: %s, polls %lu, i2c errors %lu, presses %lu, last status 0x%02X, "
                     "last point %u,%u, late retries %lu\r\n",
                     s_kind == TP_GT911 ? "GT911" : s_kind == TP_ST7123 ? "ST712x" : "none",
                     (unsigned long)s_polls, (unsigned long)s_errors, (unsigned long)s_presses,
                     s_last_status, s_last_x, s_last_y, (unsigned long)s_retries);
    return k < 0 ? 0 : ((size_t)k < len ? (size_t)k : len);
}
