/*
 * MP3 player + SD-card USB drive + Wi-Fi (via the on-board ESP32-C6) for ESP32-P4 boards:
 *   - Waveshare ESP32-P4-WIFI6-Touch-LCD-4B (720x720 MIPI-DSI + GT911, ES8311 speaker, microSD)
 *   - M5Stack Tab5 (720x1280 MIPI-DSI, ES8388 speaker, microSD)
 * Select the board in src/bsp/bsp_config.h.
 *
 * Arduino IDE settings (Tools menu), both boards:
 *   Board:            ESP32P4 Dev Module (esp32 core 3.3.x)
 *   PSRAM:            Enabled                 <- required
 *   Flash Size:       16MB (128Mb)          <- Tab5 has 16 MB; the 4B's 32 MB chip runs fine as 16 MB
 *   Partition Scheme: Custom   <- partitions.csv in this folder (15.9 MB app) is used for every build;
 *                                 "Custom" just makes the IDE's size check allow it
 *   Chip Variant:     match your chip (most 4B boards ship pre-v3.00 silicon; check your Tab5's)
 *   USB Mode:         USB-OTG (TinyUSB)       <- required for the SD-card USB drive
 *   USB CDC/MSC On Boot: Disabled (Serial stays on the USB-UART port)
 *
 * Libraries: lvgl 9.6.0 in ~/Arduino/libraries/lvgl,
 *            lv_conf.h in ~/Arduino/libraries/lv_conf.h.
 */
#ifndef BOARD_HAS_PSRAM
#error "Enable PSRAM in the Tools menu"
#endif

#include <lvgl.h>
#include "src/bsp/bsp_board.h"
#include "src/usb_msc.h"
#include "src/settings.h"
#include "src/app_ui.h"

void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println(BSP_BOARD_NAME " MP3 player");

  esp_err_t err = bsp_lvgl_start();
  if (err != ESP_OK) {
    Serial.printf("BSP init failed: %s\n", esp_err_to_name(err));
    while (true) delay(1000);
  }
  Serial.printf("Panel: %s\n", bsp_display_panel_name());

  /* Speaker on (silent). The MP3 player re-clocks it per file. */
  err = bsp_audio_init(44100);
  if (err != ESP_OK) {
    Serial.printf("Speaker init failed: %s\n", esp_err_to_name(err));
  }

  /* microSD card -> USB drive on the USB OTG port, unless switched off in
   * Settings (it can also be switched on later without a reboot). */
  if (settings_get()->usb_drive) {
    err = usb_msc_start();
    if (err == ESP_OK) {
      Serial.println("USB mass storage: SD card exported");
    } else {
      Serial.printf("USB mass storage not started (no SD card?): %s\n", esp_err_to_name(err));
    }
  } else {
    Serial.println("USB mass storage: off (Settings)");
  }

  /* Home page (default) -> MP3 player for *.mp3/*.opus on the SD card, settings. */
  err = app_ui_start();
  if (err != ESP_OK) {
    Serial.printf("MP3 player failed to start: %s\n", esp_err_to_name(err));
  }

  /* internal RAM is the scarce kind (SD + Wi-Fi DMA need it): show the margin */
  Serial.printf("Free internal RAM: %u KB (DMA-capable %u KB, largest block %u KB), PSRAM %u KB\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) >> 10),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) >> 10),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10));
}

void loop()
{
  uint32_t wait_ms = lv_timer_handler();
  delay(wait_ms < 5 ? 1 : (wait_ms > 20 ? 20 : wait_ms));
}
