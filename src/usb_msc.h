#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Exposes the microSD card as a USB mass-storage drive on the USB OTG
 * (high-speed) port. Requires Tools > USB Mode = "USB-OTG (TinyUSB)".
 * The firmware also reads the card (music player); see bsp_sdcard.c for how
 * the two share it.
 * Returns an error (and starts nothing) if no card is inserted at boot. */
esp_err_t usb_msc_start(void);

/* Runtime switch (LVGL task). Off: the drive reports "no media", so the PC
 * unmounts it and the firmware has the card to itself; USB stays connected.
 * On: starts the drive if it was never started (no reboot needed), else
 * reports the media present again. Returns the start error. */
esp_err_t usb_msc_set_enabled(bool on);

typedef enum { USB_MSC_OFF, USB_MSC_ON, USB_MSC_NO_CARD, USB_MSC_FAILED } usb_msc_state_t;
usb_msc_state_t usb_msc_state(void);

#ifdef __cplusplus
}
#endif
