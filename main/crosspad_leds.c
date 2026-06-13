// SPDX-License-Identifier: MIT
#include "crosspad_leds.h"

#include "led_strip.h"
#include "esp_log.h"

#define TAG "cp_leds"

#define CP_LED_GPIO   17
#define CP_LED_COUNT  16

// Pad index (0-based, = printed PAD label - 1) -> physical LED index on the
// WS2812 chain. Verbatim from the CrossPad BSP (CrosspadPlatform.cpp).
static const uint8_t PAD_TO_LED[16] = {
    12, 11, 4, 3, 13, 10, 5, 2, 14, 9, 6, 1, 15, 8, 7, 0
};

// Pads to light, by printed label. Cross d-pad = red, function cluster = blue.
static const uint8_t CROSS_PADS[]    = { 10, 2, 5, 7, 6 };          // up,down,left,right,fire
static const uint8_t FUNCTION_PADS[] = { 13, 14, 15, 16, 12, 8, 4 };

static led_strip_handle_t s_strip = NULL;

static void set_pad(uint8_t label, uint8_t r, uint8_t g, uint8_t b) {
    if (label < 1 || label > 16) return;
    uint8_t phys = PAD_TO_LED[label - 1];
    led_strip_set_pixel(s_strip, phys, r, g, b);
}

void crosspad_leds_init(void) {
    led_strip_config_t strip_cfg = {0};
    strip_cfg.strip_gpio_num   = CP_LED_GPIO;
    strip_cfg.max_leds         = CP_LED_COUNT;
    strip_cfg.led_pixel_format = LED_PIXEL_FORMAT_GRB;
    strip_cfg.led_model        = LED_MODEL_WS2812;
    strip_cfg.flags.invert_out = false;

    led_strip_rmt_config_t rmt_cfg = {0};
    rmt_cfg.clk_src        = RMT_CLK_SRC_DEFAULT;
    rmt_cfg.resolution_hz  = 10 * 1000 * 1000;
    rmt_cfg.flags.with_dma = false;

    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip) != ESP_OK) {
        ESP_LOGW(TAG, "LED strip init failed");
        s_strip = NULL;
        return;
    }

    led_strip_clear(s_strip);
    for (unsigned i = 0; i < sizeof(CROSS_PADS); i++)    set_pad(CROSS_PADS[i],    60, 0, 0);  // red
    for (unsigned i = 0; i < sizeof(FUNCTION_PADS); i++) set_pad(FUNCTION_PADS[i], 0, 0, 60);  // blue
    led_strip_refresh(s_strip);
    ESP_LOGI(TAG, "pad LEDs: cross red, functions blue");
}
