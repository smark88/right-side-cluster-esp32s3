# Two firmwares live in this tree:
#   7in    -- the combined 800x480 dash (Waveshare ESP32-S3-Touch-LCD-7)
#   round  -- the original 480x480 round display (ST7701S); unchanged, kept
#             buildable so nothing is lost
# Pick with:  idf.py -DDASH_TARGET=round build     (default is 7in)
if(NOT DEFINED DASH_TARGET)
    set(DASH_TARGET "7in")
endif()

if(DASH_TARGET STREQUAL "round")
    set(DASH_SRCS
            "main.c"
            "EXIO/TCA9554PWR.c"
            "LCD_Driver/ST7701S.c"
            "Touch_Driver/GT911.c"
            "Touch_Driver/esp_lcd_touch/esp_lcd_touch.c"
            "LVGL_Driver/LVGL_Driver.c"
            "I2C_Driver/I2C_Driver.c"
            "SD_Card/SD_MMC.c"
            "Buzzer/Buzzer.c"
            "ui/ui.c"
            "ui/screens/ui_Screen1.c"
            "ui/fonts/Doto_Extra_Bold_84.c"
            "ui/fonts/Schoolbell_24.c"
            "ui/images/ui_black_boot_png.c"
            "ui/boot/boot_screen.c")
    set(DASH_INCS
            "./EXIO" "./LCD_Driver" "./Touch_Driver" "./Touch_Driver/esp_lcd_touch"
            "./LVGL_Driver" "./I2C_Driver" "./Buzzer" "./ui" ".")
else()
    set(DASH_SRCS
            "main7.c"
            "board7/ch422g.c"
            "board7/lcd7.c"
            "board7/lvgl7.c"
            "ui7/dash7.c"
            "ui7/fonts/ui_font_rpm_96.c"
            "ui7/images/c5r_blue_boot.c"
            "demo/dash_demo7.c"
            "canbus/canbus7.c"
            "canbus/obd_poll.c")
    set(DASH_INCS "./board7" "./ui7" "./demo" "./canbus" ".")
endif()

idf_component_register(
    SRCS         ${DASH_SRCS}
    INCLUDE_DIRS ${DASH_INCS}
    REQUIRES
            driver
            esp_timer
            esp_lcd
            fatfs
            vfs
            freertos
            esp_wifi
            spi_flash
            nvs_flash
            bt
            esp_adc
            lvgl
)
