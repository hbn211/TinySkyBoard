#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define FORECAST_DAYS 5   // today + 4 days

typedef struct {
    int wday;           // 0 = Sunday
    int code;           // Open-Meteo WMO code
    float temp_min;
    float temp_max;
} forecast_day_t;

typedef struct {
    float temp;
    int humidity;
    float wind_kmh;
    int code;
    forecast_day_t days[FORECAST_DAYS];
    int n_days;
    time_t updated_at;
} weather_t;

#define PORTAL_AP_SSID  "TinySkyBoard"
#define PORTAL_AP_IP    "192.168.4.1"

// ui.c — call with the LVGL lock held (lvgl_port_lock)
void ui_init(bool config_mode);
void ui_rebuild(void);   // rebuilds the screen for the display's current orientation
void ui_set_weather(const weather_t *w);
void ui_set_status(const char *text);
void ui_set_fahrenheit(bool on);
void ui_toast(const char *text);          // temporary message in the status bar
void ui_config_message(const char *text);  // footer of the setup-mode screen
// messages: the cycle and new ones come from schedule.c; the button browses them
bool ui_browse_next(void);        // click: next message, then the setup card (false = back to the dashboard)
typedef enum { UI_HOLD_NONE, UI_HOLD_DELETE, UI_HOLD_LAN_SETUP } ui_hold_t;
ui_hold_t ui_hold_action(void);   // what holding the button does on the current screen
void ui_hold_progress(int percent);   // bar while holding (red = delete, blue = open setup); -1 hides it
void ui_delete_selected(void);
void ui_lan_setup_show(const char *url);   // QR code to the setup page on the home network
void ui_lan_setup_close(void);
bool ui_lan_setup_active(void);
bool ui_text_fits(const char *text, int *max_chars);   // fits the screen in any rotation?
uint32_t ui_weather_color(int code);   // RGB color 0xRRGGBB for the WMO code
const char *ui_weather_text(int code);  // description in the active language

// led.c — RGB LED on the back of the board
void led_start(void);
void led_set_alert(bool on);   // unread messages: white blink until they're opened
void led_set_alert_level(uint8_t percent, bool preview);   // flash brightness; preview = show it now
void led_solid(uint32_t rgb);
void led_set_level(uint8_t percent);   // max brightness, 0 = off

// dimmer.c — screen/LED brightness by day and night (sunrise/sunset)
#define DIM_DEFAULT_DAY    80
#define DIM_DEFAULT_NIGHT  20
#define DIM_DEFAULT_LED    35
#define DIM_DEFAULT_LED_NIGHT 8
#define DIM_DEFAULT_ALERT  50   // new-message flash, 0 = no flash

// screen cycle: seconds each screen stays (setup page; NVS keys t_dash, t_sens, t_msg)
#define CYCLE_DEFAULT_S    5
#define CYCLE_MIN_S        2
#define CYCLE_MAX_S        30
void dimmer_start(bool follow_sun);     // false = only applies the day brightness (setup mode)
void dimmer_set_sun(time_t sunrise, time_t sunset);

// net.c
void net_start(void);
// HTTPS + JSON parse of the response; GET if post_body is NULL, otherwise POST with a JSON body.
// NULL on error. Free with cJSON_Delete.
struct cJSON *net_http_json(const char *url, const char *post_body, int timeout_ms);
// Same, with one extra request header (e.g. an API key)
struct cJSON *net_http_json_hdr(const char *url, const char *post_body, const char *hdr, const char *hdr_value,
                                int timeout_ms);
void net_url_encode(const char *in, char *out, size_t size);
bool net_get_weather(weather_t *out);   // last downloaded weather

// telegram.c — receives bot messages and shows them on the panel
void telegram_start(void);

// govee.c — Govee thermometers (cloud OpenAPI)
// The API has no timestamp and keeps answering online with the last value after a sensor dies,
// so a reading counts only when it changes. The task finds when a sensor uploads (polling every
// GOVEE_FAST_S), then sleeps until just before the next upload (interval from the setup page).
#define GOVEE_MAX            8
#define GOVEE_FAST_S         5    // polling while waiting for an upload
#define GOVEE_DEFAULT_MIN    10   // the sensor's upload interval (Govee app setting), NVS "govee_int"
#define GOVEE_STALE_UPLOADS  3    // no change in this many intervals = gone from the screen
typedef struct {
    char name[32];
    float temp_c;
    float humidity;         // -1 = the sensor has none
    time_t changed_at;      // when a new reading was seen; 0 = none since power-on yet
    float day_min, day_max; // today's range (seen by the panel since midnight or since it powered on)
    int trend;              // temperature over the last hour: 1 rising, -1 falling, 0 steady
    bool has_trend;         // false until there's ~30 min of history
    bool shown;             // alive (on the screen); false = no new reading for a while
} govee_sensor_t;
void govee_start(void);
// sensors with a reading, in the account's order; all = false: only the ones to show on the screen
int govee_get(govee_sensor_t *out, int max, bool all);
uint32_t govee_version(void);   // changes when there's new data (UI polls it)

// portal.c — setup mode: own Wi-Fi + web page
void portal_start(void);
// Same page on the home network, opened from the button; closes after PORTAL_LAN_TIMEOUT_MS
#define PORTAL_LAN_TIMEOUT_MS   (10 * 60 * 1000)
bool portal_lan_start(char *url, size_t size);   // url = address for the QR code
void portal_lan_stop(void);
