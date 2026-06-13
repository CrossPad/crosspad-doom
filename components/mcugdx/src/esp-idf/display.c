// CrossPad display backend for mcugdx — esp_lcd + esp_lcd_ili9341, matching the
// CrossPad BSP exactly (the BSP renders correct geometry AND colours, so this is
// the proven path; the old nopnop2002 raw-SPI driver scrambled this panel's
// colours irrecoverably).
//
// mcugdx keeps drawing into display.frame_buffer (PSRAM). mcugdx_display_show()
// pushes it to the panel in horizontal stripes via a small internal DMA bounce
// buffer (esp_lcd can't reliably DMA straight from PSRAM here).
//
// Panel config (components/bsp/crosspad): ILI9341 320x240, SPI3 @40MHz, BGR,
// 16bpp, MOSI=47 SCK=14 CS=13 DC=21 RST=18, custom vendor init + mirror(1,1).
// NOTE: no esp_lcd_panel_reset() — the BSP skips it (RST shared w/ touch,
// reset_active_high=1 would hold an active-low panel in reset -> white).

#include "display.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"

#define TAG "mcugdx_display"

#define CP_LCD_MOSI     47
#define CP_LCD_SCK      14
#define CP_LCD_CS       13
#define CP_LCD_DC       21
#define CP_LCD_RST      18
#define CP_LCD_SPI_HOST SPI3_HOST
#define CP_LCD_PCLK_HZ  (40 * 1000 * 1000)
#define CP_LCD_H_RES    320
#define CP_LCD_V_RES    240
#define CP_STRIPE_ROWS  24

static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0xC8, (uint8_t []){0xFF, 0x93, 0x42}, 3, 0},
    {0xC0, (uint8_t []){0x0E, 0x0E}, 2, 0},
    {0xC5, (uint8_t []){0xD0}, 1, 0},
    {0xC1, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39, 0x48, 0x02, 0x0a, 0x08, 0x17, 0x17, 0x0F}, 15, 0},
    {0xE1, (uint8_t []){0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33, 0x52, 0x04, 0x0f, 0x0e, 0x37, 0x38, 0x0F}, 15, 0},
    {0xB1, (uint8_t []){00, 0x1B}, 2, 0},
    {0x36, (uint8_t []){0x08}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},
    {0xB7, (uint8_t []){0x06}, 1, 0},
    {0x11, (uint8_t []){0}, 0x80, 0},
    {0x29, (uint8_t []){0}, 0x80, 0},
    {0, (uint8_t []){0}, 0xff, 0},
};

mcugdx_display_t display;

static esp_lcd_panel_handle_t    s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io    = NULL;
static SemaphoreHandle_t         s_flush_done = NULL;
static uint16_t                 *s_bounce = NULL;

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *ctx) {
    (void)io; (void)edata; (void)ctx;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &hp);
    return hp == pdTRUE;
}

bool mcugdx_display_init(mcugdx_display_config_t *display_cfg) {
    (void)display_cfg;

    display.native_width  = CP_LCD_H_RES;
    display.native_height = CP_LCD_V_RES;
    display.width         = CP_LCD_H_RES;
    display.height        = CP_LCD_V_RES;
    display.orientation   = MCUGDX_LANDSCAPE;

    display.frame_buffer = (uint16_t *) heap_caps_malloc(
        (size_t)CP_LCD_H_RES * CP_LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_bounce = (uint16_t *) heap_caps_malloc(
        (size_t)CP_LCD_H_RES * CP_STRIPE_ROWS * sizeof(uint16_t),
        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!display.frame_buffer || !s_bounce) {
        ESP_LOGE(TAG, "fb/bounce alloc failed");
        return false;
    }
    memset(display.frame_buffer, 0, (size_t)CP_LCD_H_RES * CP_LCD_V_RES * sizeof(uint16_t));
    s_flush_done = xSemaphoreCreateBinary();

    // Manual hardware reset — the panel RST (GPIO18) is ACTIVE-LOW (per the
    // nopnop2002 driver that successfully drove this panel: pulse high->low->high).
    // esp_lcd's reset_active_high=1 would be the wrong polarity, so we reset by
    // hand here and tell esp_lcd not to touch RST (reset_gpio_num = -1). Without a
    // proper reset the panel stays in an undefined state -> white screen.
    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << CP_LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(CP_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(CP_LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));  // assert (active-low)
    gpio_set_level(CP_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(150)); // release

    const spi_bus_config_t buscfg = {
        .sclk_io_num = CP_LCD_SCK,
        .mosi_io_num = CP_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = CP_LCD_H_RES * CP_STRIPE_ROWS * sizeof(uint16_t) + 64,
    };
    if (spi_bus_initialize(CP_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed");
        return false;
    }

    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = CP_LCD_DC,
        .cs_gpio_num = CP_LCD_CS,
        .pclk_hz = CP_LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
    };
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)CP_LCD_SPI_HOST, &io_config, &s_io) != ESP_OK) {
        ESP_LOGE(TAG, "new_panel_io_spi failed");
        return false;
    }

    const ili9341_vendor_config_t vendor_config = {
        .init_cmds = &vendor_specific_init[0],
        .init_cmds_size = sizeof(vendor_specific_init) / sizeof(ili9341_lcd_init_cmd_t),
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,  // we reset RST manually above (active-low)
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,  // DOOM emits standard RGB565
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    if (esp_lcd_new_panel_ili9341(s_io, &panel_config, &s_panel) != ESP_OK) {
        ESP_LOGE(TAG, "new_panel_ili9341 failed");
        return false;
    }

    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_mirror(s_panel, true, true);
    esp_lcd_panel_disp_on_off(s_panel, true);

    ESP_LOGI(TAG, "CrossPad ILI9341 %dx%d up (esp_lcd)", CP_LCD_H_RES, CP_LCD_V_RES);
    return true;
}

void mcugdx_display_set_orientation(mcugdx_display_orientation_t orientation) {
    (void)orientation;
    display.orientation = MCUGDX_LANDSCAPE;
}

void mcugdx_display_show(void) {
    if (!s_panel) return;
    for (int y = 0; y < CP_LCD_V_RES; y += CP_STRIPE_ROWS) {
        int rows = (y + CP_STRIPE_ROWS <= CP_LCD_V_RES) ? CP_STRIPE_ROWS : (CP_LCD_V_RES - y);
        memcpy(s_bounce, display.frame_buffer + (size_t)y * CP_LCD_H_RES,
               (size_t)rows * CP_LCD_H_RES * sizeof(uint16_t));
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, CP_LCD_H_RES, y + rows, s_bounce);
        xSemaphoreTake(s_flush_done, portMAX_DELAY);
    }
}
