# esp32p4_86box

A touch-screen music player and picture frame for ESP32-P4 boards with an ESP32-C6 for Wi-Fi:

- **Waveshare ESP32-P4-WIFI6-Touch-LCD-4B**: 4" 720×720 MIPI-DSI panel, GT911 touch, ES8311 speaker, microSD.
- **M5Stack Tab5**: 5" 720×1280 MIPI-DSI panel, ES8388 speaker, microSD.

The UI is built with LVGL 9.6 as an Arduino sketch, on the bare esp32 Arduino core (no M5 or Waveshare libraries).

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

| Part      | Waveshare 4B                                  | M5Stack Tab5                                              |
|-----------|-----------------------------------------------|-----------------------------------------------------------|
| Display   | 720×720 MIPI-DSI (ST7703 or JD9365)           | 720×1280 MIPI-DSI (ILI9881C, ST7123 or ST7121)            |
| Touch     | GT911                                         | GT911 (ILI9881C panel) or built into ST7123/ST7121        |
| Audio     | ES8311 codec + amplifier                      | ES8388 codec + amplifier                                  |
| Power     | GPIOs                                         | Two PI4IOE5V6408 I/O expanders (resets, amp, C6 power)    |
| Wi-Fi     | ESP32-C6 via ESP-Hosted (SDIO, core default pins) | ESP32-C6 via ESP-Hosted (SDIO on GPIO8–13, reset 15)  |
| Storage   | microSD                                       | microSD                                                   |

The panel is detected at boot on both boards. Select the board in `src/bsp/bsp_config.h`; the Waveshare 4B is the default.

## Building

1. Install **esp32 Arduino core 3.3.x** in the Arduino IDE.
2. Install **LVGL 9.6.0** through the Library Manager, then put `lv_conf.h` at `~/Arduino/libraries/lv_conf.h`. The Monitor page needs `LV_USE_PERF_MONITOR 1` in that file.
3. Pick your board in `src/bsp/bsp_config.h`.
4. Choose these settings in the Tools menu:

   | Setting               | Value                                     |
   |-----------------------|-------------------------------------------|
   | Board                 | ESP32P4 Dev Module                        |
   | PSRAM                 | Enabled (required)                        |
   | Flash Size            | 16MB (128Mb), on both boards (the 4B's 32 MB chip works as 16 MB) |
   | Partition Scheme      | Custom (uses `partitions.csv`: one 15.9 MB app, no OTA/filesystem) |
   | Chip Variant          | Match your chip (most 4B boards are pre-v3.00) |
   | USB Mode              | USB-OTG (TinyUSB) (required for the USB drive) |
   | USB CDC/MSC On Boot   | Disabled                                  |

5. Open `esp32p4_86box.ino` and upload. On the 4B, serial output (115200 baud) goes to the USB-UART port.

## SD card layout

```
/Music/        *.mp3, *.opus   (subfolders allowed)
/Animation/    *.jpg, *.jpeg, *.gif   (subfolders allowed)
```

## Source layout

| Path                     | Contents                                                       |
|--------------------------|----------------------------------------------------------------|
| `esp32p4_86box.ino`      | Startup: BSP, audio, USB drive, app UI, LVGL loop               |
| `src/bsp/`               | Board support: board selection (`bsp_config.h`), display, touch, backlight, power/I/O expanders, ES8311/ES8388 audio, SD card, LVGL glue |
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
- M5Stack Tab5 panel init tables (Apache-2.0, from M5Stack's M5Tab5-UserDemo BSP components), in `src/bsp/bsp_tab5_panel_init.h`

## License

MIT; see [LICENSE](LICENSE).
