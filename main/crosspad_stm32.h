// SPDX-License-Identifier: MIT
//
// Minimal CrossPad STM32 co-processor driver for the standalone DOOM firmware.
//
// On CrossPad (rev2) the LCD backlight and the 16 pressure pads + rotary encoder
// are owned by an STM32 co-processor, reachable over I2C @ 0x42 with a 16-bit
// big-endian register pointer. The full firmware drives this via the heavy
// `crosspad` BSP; here we only need three things, so this is a ~self-contained
// i2c_master driver (no BSP dependency):
//   - turn the backlight on (else the screen stays black),
//   - read the 16-pad bitmap (DOOM movement/fire/use),
//   - read the encoder detent + buttons.
//
// Register map mirrors components/stm32_hal/include/stm32_reg_map.h in the
// platform-idf repo.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the I2C bus (SCL=GPIO11, SDA=GPIO12) and probe the STM32 @ 0x42.
// Returns false if the link doesn't answer (DOOM still runs, just no input).
bool crosspad_stm32_init(void);

// Set LCD backlight 0..100 (%). The STM32 drives the actual backlight rail.
void crosspad_stm32_set_brightness(uint8_t pct);

// Read the 16-pad pressed bitmap (bit i set => pad i pressed). Returns the
// last known value on a transient I2C error.
uint16_t crosspad_stm32_read_pads(void);

// Read the encoder register: low 4 bits = detent index 0..15, bit7 = enc button.
uint8_t crosspad_stm32_read_encoder(void);

// Read the side buttons register: bit0 = power button, bit1 = encoder button.
uint8_t crosspad_stm32_read_buttons(void);

#ifdef __cplusplus
}
#endif
