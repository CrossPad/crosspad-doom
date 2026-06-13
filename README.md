# crosspad-doom

DOOM (via [doomgeneric](https://github.com/ozkl/doomgeneric)) running on **CrossPad**
hardware, built on Mario Zechner's [mcugdx](https://github.com/badlogic/mcugdx)
framework. Standalone ESP-IDF firmware — separate from the main CrossPad app.

## Hardware mapping (CrossPad rev2)
- **Display:** ILI9341 320x240 SPI — MOSI=47, SCK=14, CS=13, DC=21, RST=18 (mcugdx display, landscape).
- **Backlight + input:** STM32 co-processor over I2C @0x42 (SCL=11, SDA=12). See `main/crosspad_stm32.c`.
- **WAD:** `data/Doom1.WAD` (shareware) baked into the `rofs` flash partition at build time.
- **Audio:** disabled for now (upstream I2S pins collide with the LCD; ES8388 bring-up is a TODO).

## Controls (4x4 pad grid, top-left = pad 0)
| pad | action      | pad | action      |
|-----|-------------|-----|-------------|
| 0   | ESC (menu)  | 3   | FIRE        |
| 1   | forward     | 11  | USE (open)  |
| 8   | turn left   | 12  | ENTER       |
| 9   | back        | 10  | turn right  |

## Build & flash
```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor   # console is on the native USB-Serial/JTAG
```

## Status
First milestone: render DOOM on the LCD + pad input. Audio, encoder-turn, and
save-games are follow-ups.
