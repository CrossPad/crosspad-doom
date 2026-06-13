// SPDX-License-Identifier: MIT
//
// CrossPad pad LEDs (16x WS2812B on GPIO17, driven by the ESP via RMT).
// Lights the DOOM control layout: cross d-pad red, function cluster blue.

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Init the LED strip and paint the static DOOM control layout. Safe no-op if
// the strip can't be created.
void crosspad_leds_init(void);

#ifdef __cplusplus
}
#endif
