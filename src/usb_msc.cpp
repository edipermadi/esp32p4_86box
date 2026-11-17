/*
 * microSD card -> USB mass storage, using the Arduino core's TinyUSB USBMSC class.
 * TinyUSB on the P4 runs on the OTG high-speed controller (rhport 1).
 */
#include "usb_msc.h"
#include "bsp/bsp_board.h"
#include "USB.h"
#include "USBMSC.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"

#if ARDUINO_USB_MODE
#error "Set Tools > USB Mode to \"USB-OTG (TinyUSB)\" for USB mass storage"
#endif

static const char *TAG = "usb_msc";

static USBMSC s_msc;
static sdmmc_card_t *s_card = nullptr;
static uint32_t s_sector_size = 512;
static bool s_started;
static volatile bool s_enabled;          /* checked by the TinyUSB callbacks */
static usb_msc_state_t s_state = USB_MSC_OFF;

/* TinyUSB passes whole sectors in practice (bufsize = n * 512, offset = 0);
 * reject anything else rather than silently corrupting the card. */
static int32_t on_read(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    if (!s_enabled || offset != 0 || bufsize % s_sector_size) {
        return -1;
    }
    esp_err_t err = bsp_sdcard_read(buffer, lba, bufsize / s_sector_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read lba %lu failed: %s", (unsigned long)lba, esp_err_to_name(err));
        return -1;
    }
    return (int32_t)bufsize;
}

static int32_t on_write(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    if (!s_enabled || offset != 0 || bufsize % s_sector_size) {
        return -1;
    }
    esp_err_t err = bsp_sdcard_write(buffer, lba, bufsize / s_sector_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write lba %lu failed: %s", (unsigned long)lba, esp_err_to_name(err));
        return -1;
    }
    return (int32_t)bufsize;
}

static bool on_start_stop(uint8_t power_condition, bool start, bool load_eject)
{
    if (load_eject && !start) {
        ESP_LOGI(TAG, "ejected by host");
        s_msc.mediaPresent(false);
    }
    return true;
}

extern "C" esp_err_t usb_msc_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    esp_err_t err = bsp_sdcard_init(&s_card);
    if (err != ESP_OK) {
        s_state = USB_MSC_NO_CARD;
        return err;
    }
    s_sector_size = s_card->csd.sector_size;

    s_msc.vendorID("ESP32P4");
    s_msc.productID("86Box SD Card");
    s_msc.productRevision("1.0");
    s_msc.onRead(on_read);
    s_msc.onWrite(on_write);
    s_msc.onStartStop(on_start_stop);
    s_msc.mediaPresent(true);
    s_msc.isWritable(true);
    s_enabled = true;
    if (!s_msc.begin((uint32_t)s_card->csd.capacity, (uint16_t)s_sector_size) || !USB.begin()) {
        s_enabled = false;
        s_state = USB_MSC_FAILED;
        return ESP_FAIL;
    }
    s_started = true;
    s_state = USB_MSC_ON;
    ESP_LOGI(TAG, "USB drive ready (%lu sectors)", (unsigned long)s_card->csd.capacity);
    return ESP_OK;
}

extern "C" esp_err_t usb_msc_set_enabled(bool on)
{
    if (!s_started) {
        return on ? usb_msc_start() : ESP_OK;
    }
    s_enabled = on;
    s_msc.mediaPresent(on);
    s_state = on ? USB_MSC_ON : USB_MSC_OFF;
    return ESP_OK;
}

extern "C" usb_msc_state_t usb_msc_state(void)
{
    return s_state;
}
