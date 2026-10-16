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

static void led_task(void *arg)
{
    uint32_t t = 0;
    while (true) {
        t += STEP_MS;
        if (s_alert && t % ALERT_PERIOD_MS < ALERT_ON_MS) {
            board_led_set(s_max, s_max, s_max);
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
