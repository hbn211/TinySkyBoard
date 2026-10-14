#include "app.h"
#include "board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define STEP_MS         50

static volatile uint32_t s_color;
static volatile uint8_t s_max = 90;   // current max brightness (0-255); the WS2812 is very bright up close
static volatile int s_notify;          // pending blink phases

static void led_task(void *arg)
{
    while (true) {
        if (s_notify > 0) {
            uint8_t v = (s_notify % 2) ? 0 : s_max;
            board_led_set(v, v, v);
            s_notify--;
            vTaskDelay(pdMS_TO_TICKS(150));
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

void led_notify(void)
{
    s_notify = 6;   // 3 white blinks
}
