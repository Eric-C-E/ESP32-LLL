# Live Language Lens (ESP32-S3)

Firmware for the Live Language Lens AR headset. The ESP32-S3 captures I2S audio on button press, streams it to a Jetson for translation, and displays translated snippets on two GC9A01 LCDs (one user-facing, one outward-facing).

## Hardware
- MCU: ESP32-S3 DevKitC
- Displays: Waveshare GC9A01 1.28" LCD (2x)
- Wiring (current):
  - MOSI: GPIO 11
  - SCLK: GPIO 12
  - CS: GPIO 10
  - DC: GPIO 8
  - RST: -1 (not wired)
  - MISO: -1 (not used)

## Build and Flash
```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

## Project Layout
- `main/`: application code (display task, app runtime)
- `managed_components/`: external components (GC9A01 driver, etc.)
- `sdkconfig*`: project configuration

## Display Notes
This panel expects RGB565 in MSB-first byte order. The current display path swaps bytes per pixel before `esp_lcd_panel_draw_bitmap` and uses DMA-safe buffering (waits for transfer completion before reusing the buffer).

If you change panels or bit depth, revisit:
- byte order / swap
- RGB/BGR element order
- inversion setting

## Roadmap
Planned expansion includes richer AR UI and additional input/output features.
