# Live Language Lens AR Headset

Firmware for the Live Language Lens AR headset. The ESP32-S3 captures I2S audio on button press, streams it to a Jetson for translation, and displays translated snippets on two GC9A01 LCDs (one user-facing, one outward-facing).

## Hardware
- MCU: ESP32-S3 DevKitC
- Displays: Waveshare GC9A01 1.28" LCD, Random Aliexpress 3.9" LCD
- Wiring (display 1: Augmented Reality):

| Pin     | Physical | GPIO | Function                        |
| ------- | -------- | ---- | ------------------------------- |
| FSPIHD  |          | 09   | HOLD *not connected*            |
| FSPICS0 |          | 10   | SELECT                          |
| FSPID   |          | 11   | MOSI                            |
| FSPICLK |          | 12   | CLOCK                           |
| FSPIQ   |          | 13   | MISO *not connected*            |
| FSPIWP  |          | 14   | WRITE PROTECT *not connected*   |
| *RST*   | 3V3      | 3V3  | RST pin pulled high to function |
| *DC*    | 8        | 8    | DC data command mux             |

- Wiring (display 2: Towards Target Individual)

Same as above, replace CS with pin [placeholder_update_later]


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
This panel expects RGB565 in MSB-first byte order. The standard bmp flush function of the esp_lcd lib does NOT match this requirement; the current display path swaps bytes per pixel before `esp_lcd_panel_draw_bitmap` and uses DMA-safe buffering (waits for transfer completion before reusing the buffer).

If you change panels or bit depth, revisit:
- byte order / swap
- RGB/BGR element order
- inversion setting

## Roadmap
Planned expansion includes richer AR UI and additional input/output features.
