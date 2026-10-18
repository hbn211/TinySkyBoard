#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define FORECAST_DAYS 5   // today + 4 days
#define RAIN_HOURS    6   // hourly chance of rain on the details page

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
    int utc_offset;     // seconds, from Open-Meteo (DST included)
    // details page
    float feels;        // apparent temperature
    float uv, uv_max;   // now and today's max (at night "now" is always 0)
    int aqi;            // European AQI (0..100+); -1 = unknown
    int rain_pct[RAIN_HOURS];    // chance of rain for this hour and the next ones
    int rain_hour[RAIN_HOURS];   // local hour of each
    int n_rain;
} weather_t;

// "Work" page (optional, setup page): weather in another city at chosen hours and days
#define WORK_POINTS 6   // next 12 hours, every 2 hours
typedef struct work_wx {
    char name[48];
    bool ok;
    int code;
    bool is_day;
    float temp, tmax, tmin;
    int n;
    int hour[WORK_POINTS];
    int pct[WORK_POINTS];       // chance of rain
    float t[WORK_POINTS];       // temperature
} work_wx_t;

// Other cities (optional, setup page): local time and weather
#define CITIES_MAX 4
typedef struct {
    char name[32];
    bool ok;            // has data (utc_offset is valid)
    int utc_offset;
    float temp, tmax, tmin;
    int code;
    bool is_day;
} city_wx_t;

#define PORTAL_AP_SSID  "TinySkyBoard"
#define PORTAL_AP_IP    "192.168.4.1"

// ui.c — call with the LVGL lock held (lvgl_port_lock)
void ui_init(bool config_mode);
void ui_rebuild(void);   // rebuilds the screen for the display's current orientation
void ui_set_weather(const weather_t *w);
void ui_set_cities(const city_wx_t *c, int n);
void ui_set_work(const struct work_wx *w);
void ui_set_status(const char *text);
void ui_set_fahrenheit(bool on);
void ui_toast(const char *text);          // temporary message in the status bar
void ui_config_message(const char *text);  // footer of the setup-mode screen
// messages: the cycle and new ones come from schedule.c; the button browses them
bool ui_browse_next(void);        // click: next message, then the setup card (false = back to the dashboard)
typedef enum { UI_HOLD_NONE, UI_HOLD_DELETE, UI_HOLD_LAN_SETUP, UI_HOLD_NIGHT, UI_HOLD_SCHED_OPEN,
               UI_HOLD_ZONES } ui_hold_t;   // ZONES: the action depends on when the button is released
ui_hold_t ui_hold_action(void);   // what holding the button does on the current screen
void ui_hold_progress(int percent);   // bar while holding (red = delete, blue = open setup); -1 hides it
void ui_delete_selected(void);
void ui_lan_setup_show(const char *url);   // QR code to the setup page on the home network
void ui_lan_setup_close(void);
bool ui_lan_setup_active(void);
bool ui_night_active(void);
void ui_night_toggle(void);
void ui_sched_open(void);              // hold on the schedule card: browse the schedules
void ui_zone_progress(int held_ms);    // holding on a schedule: nothing / delete today / delete all / nothing
void ui_zone_release(int held_ms);   // hold on the night-mode card: on now / off for the rest of tonight   // night clock on screen (a click only wakes the panel)
bool ui_text_fits(const char *text, int *max_chars);   // fits the screen in any rotation?
uint32_t ui_weather_color(int code);   // RGB color 0xRRGGBB for the WMO code
const char *ui_weather_text(int code);  // description in the active language

// weather_icon.c — animated weather icon (46x44) on the dashboard; LVGL lock held
// kinds = T->weather indexes
enum { WX_CLEAR, WX_PARTLY, WX_CLOUDY, WX_FOG, WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_SHOWERS, WX_THUNDER };
int weather_kind(int code);   // WMO code -> WX_*
struct _lv_obj_t *weather_icon_create(struct _lv_obj_t *parent);
void weather_icon_set(struct _lv_obj_t *box, int kind, bool day);

// led.c — RGB LED on the back of the board
void led_start(void);
void led_set_alert(bool on);
void led_set_quiet(bool on);
void led_set_urgent(bool on);   // unread reminder at night: red breathing (brightness of the message flash)   // night mode: LED off (the setup page preview still works)   // unread messages: white blink until they're opened
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
#define NIGHT_DEFAULT_FROM 0    // night mode hours (NVS night_on, night_from, night_to)
#define NIGHT_DEFAULT_TO   6
#define CYCLE_DEFAULT_S    5
#define CYCLE_MIN_S        2
#define CYCLE_MAX_S        30
void dimmer_start(bool follow_sun);     // false = only applies the day brightness (setup mode)
void dimmer_set_sun(time_t sunrise, time_t sunset);
bool dimmer_is_day(void);   // between sunrise and sunset (true while unknown)

// net.c
void net_start(void);
// HTTPS + JSON parse of the response; GET if post_body is NULL, otherwise POST with a JSON body.
// NULL on error. Free with cJSON_Delete.
struct cJSON *net_http_json(const char *url, const char *post_body, int timeout_ms);
// Same, with one extra request header (e.g. an API key); status_out (optional): HTTP status, -1 = no answer
struct cJSON *net_http_json_hdr(const char *url, const char *post_body, const char *hdr, const char *hdr_value,
                                int timeout_ms, int *status_out);
void net_url_encode(const char *in, char *out, size_t size);
bool net_get_weather(weather_t *out);   // last downloaded weather

// telegram.c — receives bot messages and shows them on the panel
void telegram_start(void);
typedef enum { TG_OFF, TG_CONNECTING, TG_OK, TG_FAILING } tg_state_t;
void telegram_notify(const char *text);   // alert to the owner, sent by the bot task (any task may call it)
// A reminder with SCHED_F_TG_DONE started (closed = false: send it with a "Done" button), or that
// occurrence left the screen from the panel (closed = true: the Telegram button says so)
void telegram_reminder(uint32_t id, int64_t occ, bool closed);
// crash.c: after a crash, "reason · task · PC/RA · version" for the power-on notice (once)
bool crash_take(char *out, size_t size);
// keeps only what the panel can draw (fonts + color emoji); returns how many characters were dropped
int text_sanitize(const char *in, char *out, size_t size);
tg_state_t telegram_state(void);

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
    char device[24];        // Govee device ID
    float temp_c;
    float humidity;         // -1 = the sensor has none
    time_t changed_at;      // when a new reading was seen; 0 = none since power-on yet (not shown)
    float day_min, day_max; // today's range (seen by the panel since midnight or since it powered on)
    int trend;              // temperature over the last hour: 1 rising, -1 falling, 0 steady
    bool has_trend;         // false until there's ~30 min of history
    bool shown;             // alive (on the screen); false = no new reading for a while
} govee_sensor_t;
void govee_start(void);
// sensors with a reading, in the account's order; all = false: only the ones to show on the screen
int govee_get(govee_sensor_t *out, int max, bool all);
uint32_t govee_version(void);   // changes when there's new data (UI polls it)
typedef struct {
    bool on;            // has a key (task running)
    int http;           // last API answer: 200 ok, 401 bad key, -1 no answer, 0 none yet
    int sensors;        // thermometers on the account
    int shown;          // of those, on the screen
} govee_status_t;
govee_status_t govee_status(void);

// Telegram alerts per sensor, saved by the sensor's Govee ID (survives renames). NVS "galert".
#define GOVEE_T_NONE  INT16_MIN   // no temperature limit
#define GOVEE_H_NONE  (-1)        // no humidity limit
typedef struct {
    char device[24];        // Govee device ID ("AA:BB:CC:...")
    char name[32];          // name when saved, to list it even before the sensor is seen again
    int16_t tmin, tmax;     // tenths of a degree Celsius
    int8_t hmin, hmax;      // percent
} govee_alert_t;
int govee_alerts_load(govee_alert_t *out, int max);    // from NVS
void govee_alerts_save(const govee_alert_t *in, int n);
typedef struct {
    char device[24];
    char name[32];
} govee_ident_t;
int govee_known(govee_ident_t *out, int max);          // sensors on the account right now (empty if not running)

// Sensor comparisons (setup page): page "A x B" with the difference. NVS "cmp".
#define GOVEE_CMP_MAX 2
typedef struct {
    char a[24], b[24];      // device IDs; empty = unused
} govee_cmp_t;
int govee_cmp_load(govee_cmp_t *out);                  // returns how many are set
void govee_cmp_save(const govee_cmp_t *in);            // GOVEE_CMP_MAX entries

// portal.c — setup mode: own Wi-Fi + web page
void portal_start(void);
// Same page on the home network, opened from the button; closes after PORTAL_LAN_TIMEOUT_MS
#define PORTAL_LAN_TIMEOUT_MS   (10 * 60 * 1000)
bool portal_lan_start(char *url, size_t size);   // url = address for the QR code
void portal_lan_stop(void);
