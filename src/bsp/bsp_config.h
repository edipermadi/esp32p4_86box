/*
 * Board selection. Uncomment exactly one line (or pass -DBSP_BOARD_... to the
 * compiler). Everything board-specific in src/bsp/ keys off these.
 *
 *   BSP_BOARD_WAVESHARE_4B : Waveshare ESP32-P4-WIFI6-Touch-LCD-4B (720x720)
 *   BSP_BOARD_M5STACK_TAB5 : M5Stack Tab5 (720x1280)
 */
#pragma once

#if !defined(BSP_BOARD_WAVESHARE_4B) && !defined(BSP_BOARD_M5STACK_TAB5)
// #define BSP_BOARD_WAVESHARE_4B
#define BSP_BOARD_M5STACK_TAB5
#endif

#if defined(BSP_BOARD_WAVESHARE_4B) && defined(BSP_BOARD_M5STACK_TAB5)
#error "Select only one board in src/bsp/bsp_config.h"
#endif
