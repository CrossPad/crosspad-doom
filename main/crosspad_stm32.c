// SPDX-License-Identifier: MIT
#include "crosspad_stm32.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#define TAG "cp_stm32"

// CrossPad rev2 wiring (pinout_v2.h)
#define CP_I2C_SCL_GPIO     11
#define CP_I2C_SDA_GPIO     12
#define CP_STM32_ADDR       0x42
#define CP_I2C_HZ           400000

// STM32 register map (16-bit, big-endian pointer) — see stm32_reg_map.h
#define REG_IN_ENCODER      0x0100  // [3:0]=detent, bit7=enc_btn
#define REG_IN_PADS_HI      0x0101  // pads 8..15
#define REG_IN_PADS_LO      0x0102  // pads 0..7
#define REG_IN_BUTTONS      0x012C  // bit0=power_btn, bit1=enc_btn
#define REG_CTL_BRIGHTNESS  0x0302  // 0..100

#define XFER_TIMEOUT_MS     50

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static bool     s_ok       = false;
static uint16_t s_last_pads = 0;

static esp_err_t reg_read(uint16_t reg, uint8_t *dst, size_t len) {
    const uint8_t addr_be[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(s_dev, addr_be, sizeof(addr_be),
                                       dst, len, XFER_TIMEOUT_MS);
}

static esp_err_t reg_write_u8(uint16_t reg, uint8_t val) {
    const uint8_t buf[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), XFER_TIMEOUT_MS);
}

bool crosspad_stm32_init(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,                       // auto-pick a free port
        .sda_io_num = CP_I2C_SDA_GPIO,
        .scl_io_num = CP_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed");
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CP_STM32_ADDR,
        .scl_speed_hz = CP_I2C_HZ,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "i2c add device failed");
        return false;
    }

    // Probe: read the identity magic 'C','P','A','D' at REG_IDENTITY_BASE 0x0000.
    uint8_t magic[4] = {0};
    if (reg_read(0x0000, magic, sizeof(magic)) == ESP_OK &&
        magic[0] == 'C' && magic[1] == 'P' && magic[2] == 'A' && magic[3] == 'D') {
        ESP_LOGI(TAG, "STM32 link up @0x%02X", CP_STM32_ADDR);
        s_ok = true;
    } else {
        ESP_LOGW(TAG, "STM32 did not answer/identify — input & backlight disabled");
        s_ok = false;
    }
    return s_ok;
}

void crosspad_stm32_set_brightness(uint8_t pct) {
    if (!s_ok) return;
    if (pct > 100) pct = 100;
    reg_write_u8(REG_CTL_BRIGHTNESS, pct);
}

uint16_t crosspad_stm32_read_pads(void) {
    if (!s_ok) return 0;
    uint8_t hilo[2] = {0};               // [0]=PADS_HI (8..15), [1]=PADS_LO (0..7)
    if (reg_read(REG_IN_PADS_HI, hilo, sizeof(hilo)) == ESP_OK) {
        s_last_pads = ((uint16_t)hilo[0] << 8) | hilo[1];
    }
    return s_last_pads;
}

uint8_t crosspad_stm32_read_encoder(void) {
    if (!s_ok) return 0;
    uint8_t v = 0;
    reg_read(REG_IN_ENCODER, &v, 1);
    return v;
}

uint8_t crosspad_stm32_read_buttons(void) {
    if (!s_ok) return 0;
    uint8_t v = 0;
    reg_read(REG_IN_BUTTONS, &v, 1);
    return v;
}
