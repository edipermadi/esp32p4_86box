/*
 * microSD card -> USB mass storage on the M5Stack Tab5's USB-C socket.
 *
 * The Tab5's USB-C is wired to the P4's internal full-speed PHY 0 (GPIO24/25),
 * which by default belongs to the USB Serial/JTAG port (serial log + one-click
 * upload). The core's USB.h/USBMSC always runs TinyUSB on the high-speed
 * controller, which is not on that socket, so here TinyUSB is driven directly on
 * the full-speed OTG controller (rhport 0) and the P4's two full-speed PHYs are
 * swapped while the drive is on:
 *
 *   drive OFF : PHY 0 = USB Serial/JTAG  -> USB-C is the serial port (default)
 *   drive ON  : PHY 0 = OTG full-speed   -> USB-C is the SD-card drive (12 Mbit/s)
 *
 * PHY 1 sits on GPIO26/27, which the Tab5 uses for I2S, so whichever controller
 * is mapped to PHY 1 has its pads disabled.
 *
 * Same API and card sharing as usb_msc.cpp (the Waveshare 4B version).
 */
#include "bsp/bsp_config.h"
#if defined(BSP_BOARD_M5STACK_TAB5)
#include <string.h>
#include "usb_msc.h"
#include "bsp/bsp_board.h"
#include "tusb.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "hal/usb_wrap_ll.h"
#include "soc/usb_serial_jtag_struct.h"
#include "soc/usb_wrap_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if ARDUINO_USB_MODE
#error "Set Tools > USB Mode to \"USB-OTG (TinyUSB)\" for USB mass storage"
#endif

static const char *TAG = "usb_msc";

#define MSC_EP_OUT   0x01
#define MSC_EP_IN    0x81
#define MSC_EP_SIZE  64                     /* full-speed bulk */
#define USB_VID_ESP  0x303A
#define USB_PID_MSC  0x0002                 /* same IDs the core uses on the 4B */

static sdmmc_card_t *s_card;
static uint32_t s_sector_size = 512;
static bool s_started;                      /* TinyUSB + PHY set up */
static volatile bool s_enabled;             /* media present / USB-C is the drive */
static usb_msc_state_t s_state = USB_MSC_OFF;
static usb_phy_handle_t s_phy;

/* ----------------------------------------------------------- descriptors */
static const tusb_desc_device_t s_dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,                   /* per interface */
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID_ESP,
    .idProduct = USB_PID_MSC,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
static const uint8_t s_cfg_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0x00, 500),
    TUD_MSC_DESCRIPTOR(0, 0, MSC_EP_OUT, MSC_EP_IN, MSC_EP_SIZE),
};

static const char *const s_strings[] = { "ESP32P4", "86Box SD Card", "0001" };

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&s_dev_desc;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_cfg_desc;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t buf[32];
    size_t n;
    if (index == 0) {
        buf[1] = 0x0409;                    /* English (US) */
        n = 1;
    } else if (index <= sizeof(s_strings) / sizeof(s_strings[0])) {
        const char *s = s_strings[index - 1];
        n = strlen(s);
        if (n > 31) {
            n = 31;
        }
        for (size_t i = 0; i < n; i++) {
            buf[1 + i] = (uint8_t)s[i];
        }
    } else {
        return NULL;
    }
    buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return buf;
}

/* --------------------------------------------------------- MSC callbacks */
static void copy_padded(uint8_t *dst, const char *src, size_t len)
{
    memset(dst, ' ', len);
    size_t n = strlen(src);
    memcpy(dst, src, n < len ? n : len);
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    copy_padded(vendor_id, "ESP32P4", 8);
    copy_padded(product_id, "86Box SD Card", 16);
    copy_padded(product_rev, "1.0", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (!s_enabled || !s_card) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);   /* medium not present */
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = s_card ? (uint32_t)s_card->csd.capacity : 0;
    *block_size = (uint16_t)s_sector_size;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    if (load_eject && !start) {
        ESP_LOGI(TAG, "ejected by host");
        s_enabled = false;
    }
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return true;
}

/* TinyUSB passes whole sectors in practice (bufsize = n * 512, offset = 0);
 * reject anything else rather than silently corrupting the card. */
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    if (!s_enabled || offset != 0 || bufsize % s_sector_size) {
        return -1;
    }
    if (bsp_sdcard_read(buffer, lba, bufsize / s_sector_size) != ESP_OK) {
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    if (!s_enabled || offset != 0 || bufsize % s_sector_size) {
        return -1;
    }
    if (bsp_sdcard_write(buffer, lba, bufsize / s_sector_size) != ESP_OK) {
        return -1;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)scsi_cmd;
    (void)buffer;
    (void)bufsize;
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);  /* invalid command */
    return -1;
}

/* ------------------------------------------------------------ USB-C role */
static void usbc_role_drive(bool drive)
{
    if (drive) {
        USB_SERIAL_JTAG.conf0.usb_pad_enable = 0;   /* serial port lets go of the socket */
        usb_wrap_ll_phy_select(&USB_WRAP, 0);       /* OTG FS -> PHY 0 (USB-C) */
        usb_wrap_ll_phy_enable_pad(&USB_WRAP, true);
    } else {
        usb_wrap_ll_phy_enable_pad(&USB_WRAP, false);
        usb_wrap_ll_phy_select(&USB_WRAP, 1);       /* OTG FS -> PHY 1 (GPIO26/27, pads off) */
        USB_SERIAL_JTAG.conf0.usb_pad_enable = 1;   /* serial port back on USB-C */
    }
}

static void usb_task(void *arg)
{
    (void)arg;
    for (;;) {
        tud_task();                                 /* blocks until there is USB work */
    }
}

static esp_err_t usb_start_stack(void)
{
    usb_phy_config_t phy_cfg = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,               /* internal full-speed PHY */
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_FULL,
    };
    usbc_role_drive(true);                          /* before and after: the PHY driver */
    esp_err_t err = usb_new_phy(&phy_cfg, &s_phy);  /* may restore its default mapping */
    if (err != ESP_OK) {
        usbc_role_drive(false);
        ESP_LOGE(TAG, "USB PHY init failed: %s", esp_err_to_name(err));
        return err;
    }
    usbc_role_drive(true);

    tusb_rhport_init_t tinit = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };
    if (!tusb_init(0, &tinit)) {                    /* rhport 0 = OTG full-speed */
        usbc_role_drive(false);
        return ESP_FAIL;
    }
    if (xTaskCreate(usb_task, "usb_msc", 6144, NULL, 5, NULL) != pdPASS) {
        tud_disconnect();
        usbc_role_drive(false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------- API */
esp_err_t usb_msc_start(void)
{
    if (s_started) {
        return usb_msc_set_enabled(true);
    }
    esp_err_t err = bsp_sdcard_init(&s_card);
    if (err != ESP_OK) {
        s_state = USB_MSC_NO_CARD;
        return err;
    }
    s_sector_size = s_card->csd.sector_size;
    s_enabled = true;
    err = usb_start_stack();
    if (err != ESP_OK) {
        s_enabled = false;
        s_state = USB_MSC_FAILED;
        return err;
    }
    s_started = true;
    s_state = USB_MSC_ON;
    ESP_LOGI(TAG, "USB-C is now the SD drive (%lu sectors)", (unsigned long)s_card->csd.capacity);
    return ESP_OK;
}

esp_err_t usb_msc_set_enabled(bool on)
{
    if (!s_started) {
        return on ? usb_msc_start() : ESP_OK;
    }
    if (on) {
        s_enabled = true;
        usbc_role_drive(true);
        tud_connect();
    } else {
        s_enabled = false;
        tud_disconnect();
        vTaskDelay(pdMS_TO_TICKS(20));              /* let the host see the detach */
        usbc_role_drive(false);
    }
    s_state = on ? USB_MSC_ON : USB_MSC_OFF;
    return ESP_OK;
}

usb_msc_state_t usb_msc_state(void)
{
    return s_state;
}

#endif /* BSP_BOARD_M5STACK_TAB5 */
