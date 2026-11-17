# esp32p4_86box

A touch-screen music player and picture frame for the **Waveshare ESP32-P4-WIFI6-Touch-LCD-4B**: a 4" 720×720 MIPI-DSI panel with GT911 touch, an ES8311 speaker, microSD, and an ESP32-C6 for Wi-Fi. The UI is built with LVGL 9.6 as an Arduino sketch.

## Features

- **Home page**: a clock synced over SNTP, a mini player, and tiles for Music, Settings, Gallery and Monitor. Music keeps playing while you move between pages.
- **Music player**
  - Plays `*.mp3` (libhelix, bundled in the core) and Ogg `*.opus` (vendored libopus 1.5.2) from `/Music` on the SD card, including subfolders.
  - Shows ID3 and OpusTags metadata, including accented characters and emoji.
  - Seek, volume and mute, shuffle and repeat.
  - Fades between tracks when you change them; natural track endings are gapless.
  - The track list is virtualised and holds up to 5000 tracks.
  - A spectrum analyzer with 36 bands (40 Hz–16 kHz). Tap it to switch to a waterfall view.
- **Gallery**
  - A full-screen slideshow of `.jpg`, `.jpeg` and `.gif` files from `/Animation` on the SD card.
  - Advances every 30 s, in name order or shuffled. Animated GIFs play.
  - Baseline JPEGs use the P4's hardware decoder. Progressive JPEGs and odd sizes fall back to stb_image in software.
  - Tap the left or right half of the screen (or swipe) for the previous or next picture.
- **Monitor**: live FPS, CPU, render time and chip temperature, with graphs.
- **Settings**
  - Brightness, screen timeout, rotation (0/90/180/270) and analyzer view.
  - Wi-Fi: scan, connect, forget.
  - Time zone and 24-hour clock.
  - USB drive on/off.
  - Settings are stored in NVS.
- **USB drive**: the microSD card is exported as a USB mass-storage device on the USB OTG port. You can switch it on or off at runtime.

## Hardware

| Part      | Detail                                                        |
|-----------|---------------------------------------------------------------|
| Board     | Waveshare ESP32-P4-WIFI6-Touch-LCD-4B                         |
| Display   | 720×720 MIPI-DSI IPS (ST7703 or JD9365, detected at boot)     |
| Touch     | GT911                                                         |
| Audio     | ES8311 codec + speaker amplifier                              |
| Wi-Fi     | On-board ESP32-C6 via ESP-Hosted (SDIO)                       |
| Storage   | microSD                                                       |

## Building

1. Install **esp32 Arduino core 3.3.x** in the Arduino IDE.
2. Install **LVGL 9.6.0** through the Library Manager, then put `lv_conf.h` at `~/Arduino/libraries/lv_conf.h`. The Monitor page needs `LV_USE_PERF_MONITOR 1` in that file.
3. Choose these settings in the Tools menu:

   | Setting               | Value                                     |
   |-----------------------|-------------------------------------------|
   | Board                 | ESP32P4 Dev Module                        |
   | PSRAM                 | Enabled (required)                        |
   | Flash Size            | 32MB (256Mb)                              |
   | Partition Scheme      | 32M Flash (13MB APP/6.75MB SPIFFS)        |
   | Chip Variant          | Match your chip (most 4B boards are pre-v3.00) |
   | USB Mode              | USB-OTG (TinyUSB) (required for the USB drive) |
   | USB CDC/MSC On Boot   | Disabled                                  |

4. Open `esp32p4_86box.ino` and upload. Serial output (115200 baud) goes to the USB-UART port.

## SD card layout

```
/Music/        *.mp3, *.opus   (subfolders allowed)
/Animation/    *.jpg, *.jpeg, *.gif   (subfolders allowed)
```

## Source layout

| Path                     | Contents                                                       |
|--------------------------|----------------------------------------------------------------|
| `esp32p4_86box.ino`      | Startup: BSP, audio, USB drive, app UI, LVGL loop               |
| `src/bsp/`               | Board support: display, touch, backlight, ES8311 audio, SD card, LVGL glue |
| `src/app_ui.*`           | Page shell: home page, settings, screen timeout, clock          |
| `src/player_ui.*`, `src/mp3_player.*` | Player UI and playback engine                     |
| `src/ogg_opus.*`, `src/opus/` | Ogg demuxer and vendored libopus decoder                  |
| `src/id3.*`              | ID3 tag reader                                                  |
| `src/spectrum.*`         | Spectrum analyzer and waterfall                                 |
| `src/gallery.*`, `src/gif_dec.*`, `src/jpeg_sw.*`, `src/img_scale.*` | Slideshow and image decoding |
| `src/perf_ui.*`          | Monitor page                                                    |
| `src/wifi_mgr.*`, `src/wifi_ui.*` | Wi-Fi worker task and page                             |
| `src/settings.*`, `src/timezones.*` | NVS settings and time zone table                     |
| `src/usb_msc.*`          | SD card as a USB mass-storage device                            |
| `src/fonts/`             | Pre-generated UI, clock and emoji fonts                         |
| `src/third_party/`       | stb_image                                                       |
| `tools/vendor_opus.py`   | Regenerates `src/opus/` from an opus-1.5.2 release tarball      |

## Third-party code

- [LVGL](https://lvgl.io) (MIT), installed separately
- libopus 1.5.2 (BSD-3-Clause), vendored in `src/opus/`; see `src/opus/VENDORED.md`
- stb_image v2.30 (public domain / MIT), in `src/third_party/`

## License

MIT; see [LICENSE](LICENSE).
