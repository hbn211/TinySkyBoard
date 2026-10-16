idf_component_register(SRCS "dimmer.c" "portal_help.c" "main.c" "board.c" "ui.c" "net.c" "led.c" "dimmer.c" "config.c" "portal.c" "portal_help.c" "telegram.c" "i18n.c" "schedule.c" "govee.c"
                            "fonts/font_pt_12.c" "fonts/font_pt_14.c" "fonts/font_pt_20.c" "fonts/font_pt_28.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_lcd esp_driver_spi esp_driver_ledc esp_driver_gpio
                                     esp_wifi esp_netif esp_event nvs_flash esp_http_client esp_http_server mbedtls
                                     esp_app_format)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
