#include "ch422g.h"

#include "esp_log.h"

static const char *TAG = "CH422G";

#define I2C_SDA   8
#define I2C_SCL   9
#define I2C_HZ    400000

// The CH422G does not have registers behind one address. Each "register" IS
// its own 7-bit I2C address, and a write is a single data byte to it.
#define ADDR_SYSTEM  0x24   // mode: bit 0 = enable push-pull outputs IO0-7
#define ADDR_OUTPUT  0x38   // output levels for IO0-7

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_sys, s_out;
static uint8_t s_state;

static esp_err_t write_byte(i2c_master_dev_handle_t dev, uint8_t v)
{
    return i2c_master_transmit(dev, &v, 1, 50);
}

esp_err_t ch422g_init(bool can_enabled)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) return err;

    i2c_device_config_t dev = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .scl_speed_hz = I2C_HZ };
    dev.device_address = ADDR_SYSTEM;
    if ((err = i2c_master_bus_add_device(s_bus, &dev, &s_sys)) != ESP_OK) return err;
    dev.device_address = ADDR_OUTPUT;
    if ((err = i2c_master_bus_add_device(s_bus, &dev, &s_out)) != ESP_OK) return err;

    if ((err = write_byte(s_sys, 0x01)) != ESP_OK) {
        ESP_LOGE(TAG, "no answer at 0x%02X -- is this the 7in board?", ADDR_SYSTEM);
        return err;
    }

    // Backlight starts OFF so the boot frame is drawn before anything is
    // visible; main turns it on. Resets released, SD deselected.
    s_state = CH422G_TP_RST | CH422G_LCD_RST | CH422G_SD_CS;
    if (can_enabled)
        s_state |= CH422G_USB_SEL;

    ESP_LOGI(TAG, "outputs 0x%02X (CAN %s)", s_state, can_enabled ? "on" : "off, native USB kept");
    return write_byte(s_out, s_state);
}

esp_err_t ch422g_set(uint8_t bits, bool on)
{
    s_state = on ? (s_state | bits) : (s_state & ~bits);
    return write_byte(s_out, s_state);
}

i2c_master_bus_handle_t ch422g_bus(void) { return s_bus; }
