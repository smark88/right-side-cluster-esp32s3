#include "touch7.h"
#include "ch422g.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "TOUCH7";

#define TP_INT        GPIO_NUM_4
#define GT911_ADDR    0x5D          // selected by holding INT low through reset
#define GT911_ADDR2   0x14          // the other strap, tried if 0x5D is silent

#define REG_STATUS    0x814E        // bit 7 = new data, low nibble = points
#define REG_POINT1    0x8150        // x lo, x hi, y lo, y hi
#define REG_PRODUCT   0x8140

static i2c_master_dev_handle_t s_dev;
static lv_indev_drv_t s_drv;
static lv_coord_t s_x, s_y;
static bool s_down;

static esp_err_t rd(uint16_t reg, uint8_t *buf, size_t n)
{
    uint8_t r[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(s_dev, r, 2, buf, n, 20);
}

static esp_err_t wr8(uint16_t reg, uint8_t v)
{
    uint8_t b[3] = { reg >> 8, reg & 0xFF, v };
    return i2c_master_transmit(s_dev, b, 3, 20);
}

// LVGL polls this every LV_INDEV_DEF_READ_PERIOD from its own task.
static void read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint8_t st;
    if (rd(REG_STATUS, &st, 1) == ESP_OK && (st & 0x80)) {
        int n = st & 0x0F;
        uint8_t p[4];
        if (n > 0 && rd(REG_POINT1, p, 4) == ESP_OK) {
            s_x = p[0] | (p[1] << 8);
            s_y = p[2] | (p[3] << 8);
            s_down = true;
        } else {
            s_down = false;
        }
        wr8(REG_STATUS, 0);            // hand the buffer back to the GT911
    }
    // No new data keeps the last state: a held finger reports only on change.
    data->point.x = s_x;
    data->point.y = s_y;
    data->state = s_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static bool add_dev(uint8_t addr)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 400000,
    };
    if (i2c_master_bus_add_device(ch422g_bus(), &cfg, &s_dev) != ESP_OK)
        return false;
    char id[5] = { 0 };
    if (rd(REG_PRODUCT, (uint8_t *)id, 4) == ESP_OK) {
        ESP_LOGI(TAG, "GT911 at 0x%02X, product id \"%s\"", addr, id);
        return true;
    }
    i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return false;
}

esp_err_t touch7_init(void)
{
    // Reset with INT held low, which straps the GT911 to address 0x5D (the
    // sequence Waveshare's own demo uses on this board).
    gpio_config_t io = { .pin_bit_mask = 1ULL << TP_INT, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    ch422g_set(CH422G_TP_RST, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TP_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    ch422g_set(CH422G_TP_RST, true);
    vTaskDelay(pdMS_TO_TICKS(60));
    gpio_set_direction(TP_INT, GPIO_MODE_INPUT);

    if (!add_dev(GT911_ADDR) && !add_dev(GT911_ADDR2)) {
        ESP_LOGW(TAG, "no GT911 answering -- running without touch");
        return ESP_ERR_NOT_FOUND;
    }

    lv_indev_drv_init(&s_drv);
    s_drv.type    = LV_INDEV_TYPE_POINTER;
    s_drv.read_cb = read_cb;
    lv_indev_drv_register(&s_drv);
    return ESP_OK;
}
