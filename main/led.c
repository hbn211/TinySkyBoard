#include "app.h"
#include "board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define STEP_MS         50
#define ALERT_ON_MS     400     // unread message: white flash...
#define ALERT_PERIOD_MS 1500    // ...once per period, weather color in between

static volatile uint32_t s_color;
static volatile uint8_t s_max = 90;   // current max brightness (0-255); the WS2812 is very bright up close
static volatile bool s_alert;          // unread messages: keep blinking
static volatile uint8_t s_alert_max = 128;   // brightness of the white flash (0-255), set on the setup page
static volatile int s_preview_ms;      // setup page slider: show the flash for a moment

static void led_task(void *arg)
{
    uint32_t t = 0;
    while (true) {
        t += STEP_MS;
        if (s_preview_ms > 0) {
            s_preview_ms -= STEP_MS;
            board_led_set(s_alert_max, s_alert_max, s_alert_max);
            vTaskDelay(pdMS_TO_TICKS(STEP_MS));
            continue;
        }
        if (s_alert && t % ALERT_PERIOD_MS < ALERT_ON_MS) {
            board_led_set(s_alert_max, s_alert_max, s_alert_max);
            vTaskDelay(pdMS_TO_TICKS(STEP_MS));
            continue;
        }
        uint32_t c = s_color;
        float k = s_max / 255.0f;
        board_led_set(((c >> 16) & 0xff) * k, ((c >> 8) & 0xff) * k, (c & 0xff) * k);
        vTaskDelay(pdMS_TO_TICKS(STEP_MS));
    }
}

void led_start(void)
{
    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
}

void led_solid(uint32_t rgb)
{
    s_color = rgb;
}

void led_set_level(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_max = percent * 255 / 100;
}

void led_set_alert(bool on)
{
    s_alert = on;
}

void led_set_alert_level(uint8_t percent, bool preview)
{
    if (percent > 100) {
        percent = 100;
    }
    s_alert_max = percent * 255 / 100;
    if (preview) {
        s_preview_ms = 1200;
    }
}
