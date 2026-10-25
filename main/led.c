#include "app.h"
#include "board.h"
#include "config.h"

#include <stdlib.h>
#include <string.h>

#include <math.h>
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
static volatile bool s_quiet;          // night mode: LED off, unread blink included
static volatile bool s_urgent;         // unread reminder during night hours (red breathing by default)
#define BREATH_MS       3000
#define PREVIEW_STYLE_MS 3000

// notification look, set on the setup page: unread messages (day) and a reminder at night
static volatile uint32_t s_day_rgb = 0xffffff, s_night_rgb = 0xff0000;
static volatile uint8_t s_day_mode = LED_BLINK, s_night_mode = LED_BREATHE;
static volatile uint32_t s_prev_rgb;
static volatile uint8_t s_prev_mode;
static volatile int s_prev_style_ms;   // setup page: show a color/mode for a moment

// 0..1 for the time t in the given mode (blink: on/off; breathe: smooth, never fully dark; solid: on)
static float pattern(uint8_t mode, uint32_t t)
{
    if (mode == LED_SOLID) {
        return 1.0f;
    }
    if (mode == LED_BREATHE) {
        return 0.08f + 0.92f * (1.0f - cosf(2.0f * (float)M_PI * (t % BREATH_MS) / BREATH_MS)) / 2.0f;
    }
    return t % ALERT_PERIOD_MS < ALERT_ON_MS ? 1.0f : 0.0f;
}

static void show(uint32_t rgb, float k)
{
    board_led_set(((rgb >> 16) & 0xff) * k, ((rgb >> 8) & 0xff) * k, (rgb & 0xff) * k);
}

static void led_task(void *arg)
{
    uint32_t t = 0;
    while (true) {
        t += STEP_MS;
        if (s_prev_style_ms > 0) {
            s_prev_style_ms -= STEP_MS;
            show(s_prev_rgb, pattern(s_prev_mode, t) * (s_alert_max ? s_alert_max : 60) / 255.0f);
            vTaskDelay(pdMS_TO_TICKS(STEP_MS));
            continue;
        }
        if (s_preview_ms > 0) {
            s_preview_ms -= STEP_MS;
            show(s_day_rgb, s_alert_max / 255.0f);
            vTaskDelay(pdMS_TO_TICKS(STEP_MS));
            continue;
        }
        if (s_quiet) {
            board_led_set(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(STEP_MS));
            continue;
        }
        if (s_urgent) {
            uint8_t max = s_alert_max ? s_alert_max : 60;   // a reminder shows even with the flash set to 0%
            show(s_night_rgb, pattern(s_night_mode, t) * max / 255.0f);
            vTaskDelay(pdMS_TO_TICKS(STEP_MS));
            continue;
        }
        // unread: blink shows the weather color between flashes; breathe and solid take the LED over
        if (s_alert && s_alert_max && (s_day_mode != LED_BLINK || t % ALERT_PERIOD_MS < ALERT_ON_MS)) {
            show(s_day_rgb, pattern(s_day_mode, t) * s_alert_max / 255.0f);
            vTaskDelay(pdMS_TO_TICKS(STEP_MS));
            continue;
        }
        uint32_t c = s_color;
        float k = s_max / 255.0f;
        board_led_set(((c >> 16) & 0xff) * k, ((c >> 8) & 0xff) * k, (c & 0xff) * k);
        vTaskDelay(pdMS_TO_TICKS(STEP_MS));
    }
}

// "#rrggbb" -> 0xrrggbb; def if not a color
uint32_t led_parse_rgb(const char *s, uint32_t def)
{
    if (!s || s[0] != '#' || strlen(s) != 7) {
        return def;
    }
    char *end;
    uint32_t v = strtoul(s + 1, &end, 16);
    return *end ? def : v;
}

void led_load_styles(void)
{
    char c[8];
    config_get_str("nled_c", c, sizeof(c));
    s_day_rgb = led_parse_rgb(c, 0xffffff);
    config_get_str("nled_nc", c, sizeof(c));
    s_night_rgb = led_parse_rgb(c, 0xff0000);
    s_day_mode = config_get_u8_def("nled_m", LED_BLINK) % 3;
    s_night_mode = config_get_u8_def("nled_nm", LED_BREATHE) % 3;
}

void led_preview_style(uint32_t rgb, uint8_t mode)
{
    s_prev_rgb = rgb;
    s_prev_mode = mode % 3;
    s_prev_style_ms = PREVIEW_STYLE_MS;
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

void led_set_urgent(bool on)
{
    s_urgent = on;
}

void led_set_quiet(bool on)
{
    s_quiet = on;
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
