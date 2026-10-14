#include "app.h"
#include "board.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// Screen and LED brightness: day/night by sunrise and sunset (Open-Meteo),
// with a gradual transition around each one.

#define RAMP_S      (30 * 60)   // 30 min transition (15 before, 15 after)
#define CHECK_MS    (20 * 1000)

static const char *TAG = "dimmer";

static uint8_t s_day = DIM_DEFAULT_DAY;
static uint8_t s_night = DIM_DEFAULT_NIGHT;
static uint8_t s_led = DIM_DEFAULT_LED;
static uint8_t s_led_night = DIM_DEFAULT_LED_NIGHT;
static int s_sunrise_s = -1;    // seconds since local midnight; -1 = no data yet
static int s_sunset_s = -1;
static int s_last_screen = -1;

static int sec_of_day(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
}

// 1 = day, 0 = night, with a linear ramp around sunrise and sunset
static float day_factor(int now_s)
{
    float rise = (float)(now_s - (s_sunrise_s - RAMP_S / 2)) / RAMP_S;
    float set = (float)((s_sunset_s + RAMP_S / 2) - now_s) / RAMP_S;
    float f = rise < set ? rise : set;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

static void apply(void)
{
    float f = 1;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (s_sunrise_s >= 0 && tm.tm_year >= (2024 - 1900)) {
        f = day_factor(sec_of_day(now));
    }
    // screen and LED follow the same curve, each between its own night and day brightness
    int screen = s_night + (int)((s_day - s_night) * f + 0.5f);
    int led = s_led_night + (int)((s_led - s_led_night) * f + 0.5f);
    board_backlight_set(screen);
    led_set_level(led);
    if (screen != s_last_screen) {
        ESP_LOGI(TAG, "screen %d%%, LED %d%% (day %.2f)", screen, led, f);
        s_last_screen = screen;
    }
}

static void dimmer_task(void *arg)
{
    while (true) {
        apply();
        vTaskDelay(pdMS_TO_TICKS(CHECK_MS));
    }
}

void dimmer_start(bool follow_sun)
{
    s_day = config_get_u8_def("bl_day", DIM_DEFAULT_DAY);
    s_night = config_get_u8_def("bl_night", DIM_DEFAULT_NIGHT);
    s_led = config_get_u8_def("led_lvl", DIM_DEFAULT_LED);
    s_led_night = config_get_u8_def("led_night", DIM_DEFAULT_LED_NIGHT);
    apply();
    if (follow_sun) {
        xTaskCreate(dimmer_task, "dimmer", 3072, NULL, 2, NULL);
    }
}

void dimmer_set_sun(time_t sunrise, time_t sunset)
{
    s_sunrise_s = sec_of_day(sunrise);
    s_sunset_s = sec_of_day(sunset);
    apply();
}
