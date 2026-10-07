// CH422G I/O expander on the Waveshare ESP32-S3-Touch-LCD-7.
//
// The expander gates three things this firmware cares about: the LCD
// backlight, the touch controller's reset, and -- the one that bites -- the
// CAN transceiver. CAN shares GPIO19/20 with native USB, and USB_SEL selects
// which one is connected. With USB_SEL low the transceiver is unpowered and
// CAN is dead no matter how correct the TWAI setup is.

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

// Bits of the output register, numbered as Waveshare's EXIO labels.
#define CH422G_TP_RST   (1u << 1)
#define CH422G_DISP     (1u << 2)   // backlight enable
#define CH422G_LCD_RST  (1u << 3)
#define CH422G_SD_CS    (1u << 4)   // high = card deselected
#define CH422G_USB_SEL  (1u << 5)   // high = CAN transceiver on, native USB off

// Brings up the I2C bus (GPIO8 SDA / GPIO9 SCL, shared with the GT911 touch
// controller) and drives the outputs to a safe starting state.
esp_err_t ch422g_init(bool can_enabled);

// Sets or clears output bits, keeping the rest.
esp_err_t ch422g_set(uint8_t bits, bool on);

// The bus handle, so the touch driver can share it rather than re-install it.
i2c_master_bus_handle_t ch422g_bus(void);
