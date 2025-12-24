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
Screen 2 not integrated yet
Screens share MOSI, CLK, DC, replace CS with pin [placeholder_update_later] 

- Audio Input: dual audio input. Realistically these MEMS mics can be put anywhere you want, but I had one facing forward to capture subject in the gain pattern, as well as one facing towards the wearer, multiplexed not in hardware but in software - the parsing software will read the TCP headers containing language information and extract L or R channel into a mono stream for Whisper.

For your information, the microphones used are IMNP441s. They are drop-in for this project. 24 bit precision, 18 useable, MSB first, Phillips (1 CLK cycle delay from WS edge).

| Pin     | Physical | GPIO | Function                        |
| ------- | -------- | ---- | ------------------------------- |
| SCK     |          | 04   | CLOCK                           |
| WS      |          | 05   | SELECT                          |
| SD      |          | 06   | Serial Data                     |
| LR      |          | DIFF | Set L/R channel output          |

Microphones share SCK, WS, SD. 

Pull one L/R pin high and one low for stereo. The microphones will multiplex by taking up half the frame each in L0 R0 L1 R1 etc format.

## Build and Flash
```bash
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

## Project Layout
- `main/`: application code (task and headers)
- `managed_components/`: external components (GC9A01 driver, LVGL)
- `sdkconfig*`: project configuration. Run menuconfig to adjust access point to your edge inference/other device. Only works over ipv4.

## Display Notes
GC9A01 based panel expects RGB565 in MSB-first byte order. The standard bmp flush function of the esp_lcd lib does NOT match this requirement; the current display path swaps bytes per pixel before `esp_lcd_panel_draw_bitmap` and uses DMA-safe buffering (waits for transfer completion before reusing the buffer).

If you change panels or bit depth, revisit:
- byte order / swap
- RGB/BGR element order
- inversion setting

## Roadmap
Planned expansion includes richer AR UI and additional input/output features.
