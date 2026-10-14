#include "app.h"
#include "board.h"
#include "config.h"
#include "i18n.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

#define CLICK_MAX_MS      400     // up to this it's a click: next message
#define DELETE_START_MS   300     // holding on a message: the delete bar appears...
#define DELETE_MS         2000    // ...and it deletes here
#define HINT_PRESS_MS     2000    // on the dashboard: setup mode hint
#define CONFIG_PRESS_MS   5000    // enters (or leaves) setup mode

static const char *TAG = "main";

static void toast(const char *text)
{
    lvgl_port_lock(0);
    ui_toast(text);
    lvgl_port_unlock();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    config_load();
    i18n_set(config_get_u8_def("lang", LANG_EN));

    board_led_init();
    led_start();
    board_display_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    bool portal = config_take_portal_request() || !g_cfg.ssid[0];
    uint8_t rot = config_get_u8("rot") % 4;
    bool fahrenheit = config_get_u8("fahr");
    ESP_LOGI(TAG, "%s mode, network '%s', city '%s'", portal ? "setup" : "normal", g_cfg.ssid, g_cfg.city);

    lvgl_port_lock(0);
    board_display_set_rotation((lv_display_rotation_t)rot);
    ui_set_fahrenheit(fahrenheit);
    ui_init(portal);
    lvgl_port_unlock();

    dimmer_start(!portal);
    if (portal) {
        portal_start();
    } else {
        net_start();
    }

    // BOOT button (control). Rotation and unit are set on the setup page.
    //   quick click: steps through the active messages (10 s without a click goes back to the dashboard)
    //   hold 2 s on a message: deletes that message
    //   hold 5 s on the dashboard: enters (or leaves) setup mode
    const gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOOT_BTN_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    int held_ms = 0;
    bool on_message = false;   // the press started with a message selected
    bool done = false;         // this press already did its long action
    while (true) {
        bool pressed = gpio_get_level(BOOT_BTN_PIN) == 0;
        if (pressed) {
            if (held_ms == 0) {
                lvgl_port_lock(0);
                on_message = !portal && ui_browsing();
                lvgl_port_unlock();
            }
            held_ms += 20;
            if (on_message && !done && held_ms >= DELETE_START_MS) {
                lvgl_port_lock(0);
                if (held_ms >= DELETE_MS) {
                    ESP_LOGI(TAG, "held 2 s: deleting the selected message");
                    ui_delete_selected();
                    done = true;
                } else {
                    ui_delete_progress((held_ms - DELETE_START_MS) * 100 / (DELETE_MS - DELETE_START_MS));
                }
                lvgl_port_unlock();
            } else if (!on_message && held_ms == HINT_PRESS_MS) {
                toast(portal ? T->hold_exit : T->hold_config);
            } else if (!on_message && held_ms == CONFIG_PRESS_MS) {
                ESP_LOGI(TAG, "button 5 s: %s", portal ? "leaving setup mode" : "entering setup mode");
                toast(T->restarting);
                vTaskDelay(pdMS_TO_TICKS(300));
                if (portal) {
                    esp_restart();
                } else {
                    config_request_portal();
                }
            }
        } else if (held_ms > 0) {
            lvgl_port_lock(0);
            if (held_ms >= 40 && held_ms <= CLICK_MAX_MS && !portal) {
                ui_delete_progress(-1);
                ui_browse_next();
            } else if (on_message && !done) {
                ui_delete_progress(-1);   // released before 2 s: nothing is deleted
            }
            lvgl_port_unlock();
            held_ms = 0;
            done = false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
