# ESP32-LLL Agent Guide

## Project overview
Firmware for the Live Language Lens AR headset on ESP32-S3. The device captures
I2S audio while a button is held, streams it to a Jetson over TCP for
transcription/translation, and displays translated text on GC9A01 LCDs.

## Entry point
- `main/main.c` initializes NVS, networking, and starts the Wi-Fi, audio,
  display, GPIO, and TCP tasks.

## Core tasks and data flow
1. `app_audio.c` configures I2S (IMNP441, 24-bit stereo) and pushes audio frames
   into a FreeRTOS ring buffer.
2. `app_gpio.c` debounces two buttons and publishes FSM state:
   idle, translate_lang1, translate_lang2.
3. `app_tcp.c` connects to the Jetson and:
   - TX task sends audio frames with a TCP header when in translate states.
   - RX task receives text frames and routes them to display queues.
4. `app_display.c` uses esp_lcd + LVGL to render scrolling text and status.
5. `app_wifi.c` maintains Wi-Fi connection and reports RSSI.

## Protocol details
- Header struct: `msg_hdr_t` (packed) with `magic`, `version`, `msg_type`,
  `flags`, `payload_len`.
- `magic = 0xAA`, `version = 1`
- `msg_type`: audio = 1, text = 2
- `flags`: `0x01` lang1, `0x02` lang2, `0x04` screen1, `0x08` screen2
- Audio payloads are 24-bit packed PCM frames (up to 3072 bytes per chunk).
- Text payloads are UTF-8, capped at `TEXT_BUF_SIZE` (128 bytes).

## Display behavior
- Lines are wrapped and truncated to fit the GC9A01 panel.
- Each line expires after `DISPLAY_LINE_MAX_AGE_MS` (10s) so the UI scrolls.
- Indicators show RDY/REC state and Wi-Fi RSSI.
- Screen orientation: LVGL display rotations are set in `app_display.c` (screen1 uses `swap_xy=true, mirror_y=true`; screen2 uses `swap_xy=false, mirror_y=true`).
- Boot logo is drawn via `show_boot_logo()` with a 90° CCW geometry rotation to match the physical screen orientation; prefer rotating specific LVGL objects rather than swapping panel axes.

## Build and flash
```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

## Key configuration files
- `sdkconfig*`: ESP-IDF project configuration.
- `main/Kconfig.projbuild`: project-level Kconfig definitions.

## Notes
- TX task drops audio when in idle state to keep buffers from overflowing.
- RX task routes text to display queues based on header flags.
