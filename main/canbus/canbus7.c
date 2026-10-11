// CAN on the 7in board: onboard transceiver on GPIO20 (TX) / GPIO19 (RX).
//
// Only the OBD path is wired up here -- the 7in polls the car itself, so there
// is no protocol-json decoding and no gauge-to-gauge bridge.

#include "canbus.h"
#include "obd_poll.h"
#include "canbus7.h"
#include "dtc7.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "CANBUS";

#define CAN_TX GPIO_NUM_20
#define CAN_RX GPIO_NUM_19

volatile can_dash_data_t can_data = {0};

// 64-bit, written on core 0 and read on core 1: a torn read can at worst
// misjudge staleness for one 16ms frame, which the 2s window absorbs.
static volatile int64_t s_last_obd_us;

int64_t canbus_last_obd_us(void) { return s_last_obd_us; }

void process_can_frame(uint32_t id, uint8_t *data)
{
    // Trouble-code answers first: they share the 0x7E8.. ids with poll replies.
    if (dtc_handle_frame(id, data, 8))
        return;
    if (obd_poll_handle_frame(id, data, 8))
        s_last_obd_us = esp_timer_get_time();
}

void canbus_init(void)
{
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX, CAN_RX, TWAI_MODE_NORMAL);
    // A GM Global A bus runs ~1800 frames/sec; the default depth of 5 fills in
    // under 3ms, inside the window a busy frame can hold a core.
    g.rx_queue_len = 64;
    twai_timing_config_t  t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t  f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    ESP_ERROR_CHECK(twai_driver_install(&g, &t, &f));
    ESP_ERROR_CHECK(twai_start());
    ESP_LOGI(TAG, "CAN up at 500k on TX=GPIO%d RX=GPIO%d", CAN_TX, CAN_RX);
}

void canbus_task(void *arg)
{
    twai_message_t m;
    while (1) {
        if (twai_receive(&m, pdMS_TO_TICKS(50)) == ESP_OK && !m.extd && !m.rtr)
            process_can_frame(m.identifier, m.data);
    }
}
