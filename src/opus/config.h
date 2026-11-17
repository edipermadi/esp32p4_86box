/* libopus build configuration for the ESP32-P4 Arduino sketch (decoder only). */
#ifndef OPUS_VENDORED_CONFIG_H
#define OPUS_VENDORED_CONFIG_H
#define OPUS_BUILD 1
#define FIXED_POINT 1          /* integer decoder: fast and well tested on MCUs */
#define DISABLE_FLOAT_API 1    /* we only call opus_decode() (int16 output) */
#define VAR_ARRAYS 1           /* scratch on the stack: give the decoding task ~32 KB */
#define HAVE_LRINT 1
#define HAVE_LRINTF 1
#define PACKAGE_VERSION "1.5.2"
#endif
