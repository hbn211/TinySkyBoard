#include "app.h"
#include "board.h"
#include "config.h"
#include "i18n.h"
#include "schedule.h"
#include "emoji/emoji.h"
#include "src/widgets/label/lv_label_private.h"   // label offset: is a rolling text back at the top?

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"

#define COLOR_BG       lv_color_hex(0x0b0f1a)
#define COLOR_CARD     lv_color_hex(0x161d2e)
#define COLOR_MUTED    lv_color_hex(0xbac4d6)   // secondary text: light enough to read at low backlight
#define COLOR_ACCENT   lv_color_hex(0x4fc3f7)
#define COLOR_UNREAD   lv_color_hex(0xffb74d)

#define DEG "\xC2\xB0"

LV_FONT_DECLARE(font_pt_12);
LV_FONT_DECLARE(font_pt_14);
LV_FONT_DECLARE(font_pt_20);
LV_FONT_DECLARE(font_pt_28);
LV_FONT_DECLARE(font_clock_100);   // digits and ':' only, for the night clock

// cycle: dashboard, each sensor page, each active message (seconds, from the setup page)
static int s_dash_s = CYCLE_DEFAULT_S;
static int s_sens_s = CYCLE_DEFAULT_S;
static int s_msg_s = CYCLE_DEFAULT_S;
#define MSG_NEW_S      20   // a new message stays longer the first time
#define BROWSE_S       10   // browse mode (button) without a click goes back to normal
#define DELETED_MS     1200 // how long the "deleted" notice shows
#define SENS_PER_PAGE  2

// message text area: 36 px title on top, 22 px footer at the bottom
#define MSG_TOP        36
#define MSG_TOP_TALL   52   // portrait: the date/time goes on a second line under the title
#define MSG_BOTTOM     22

static lv_obj_t *s_status;
static lv_obj_t *s_sig;           // Wi-Fi signal bars next to the network name
static lv_obj_t *s_sig_bar[4];
static int s_sig_sweep = -1;      // not connected: bars light up one by one; -1 = connected
static lv_obj_t *s_time;
static lv_obj_t *s_sec;
static lv_obj_t *s_date;
static lv_obj_t *s_temp;
static lv_obj_t *s_extra;
static lv_obj_t *s_wicon;     // animated weather icon
static lv_obj_t *s_forecast;
static lv_obj_t *s_cfg_msg;
static lv_obj_t *s_msg_panel;
static lv_obj_t *s_msg_title;
static lv_obj_t *s_msg_label;
static lv_obj_t *s_msg_when;
static lv_obj_t *s_msg_hint;      // browse-mode footer
static int s_msg_top = MSG_TOP;   // where the message text area starts (MSG_TOP or MSG_TOP_TALL)
static lv_obj_t *s_del_bar;       // "hold to delete" progress
static lv_obj_t *s_del_done;      // "deleted" notice
static bool s_config_mode;

static void build_lan_panel(int w, int h);
static void night_wake(int seconds);

static weather_t s_weather;
static bool s_has_weather;
static bool s_fahrenheit;
static char s_status_text[64];
static char s_toast[48];
static uint32_t s_toast_tick;

#define TOAST_MS 2500

static msg_item_t s_items[MSG_ITEMS_MAX];   // active messages, newest first
static int s_n;
static int s_idx = -1;      // item on screen; -1 = dashboard
static int s_left = CYCLE_DEFAULT_S;   // seconds until the next switch
static bool s_browse;       // browsing with the button
static int s_browse_left;
static bool s_deleting;     // showing the "deleted" notice
static bool s_card;         // browse mode on a card after the last message: "set up over Wi-Fi"...
static bool s_card_night;   // ...or the next one, "night mode" (turn it on/off now)
static lv_obj_t *s_card_img; // moon on the night-mode card
static bool s_card_sched;   // ...then the schedule card
static bool s_sv;           // browsing the schedules (opened from the schedule card)
static sched_t s_sv_list[SCHED_MAX];
static int s_sv_n, s_sv_idx;
static lv_obj_t *s_sv_info;  // "18:00 · 30 min · every day · on the panel now"
static lv_obj_t *s_zone, *s_zone_mark, *s_zone_lbl;   // hold bar: nothing / today / all / nothing
static lv_obj_t *s_del_txt;
static bool s_sv_roll;           // the schedule on screen is long and rolls up and down
static lv_timer_t *s_roll_wait;  // time's up: waiting for the roll to get back to the top
// hold on a schedule (ms since the press): the action is decided when the button is released
#define ZONE_START     300
#define ZONE_TODAY     1200
#define ZONE_ALL       2700
#define ZONE_NONE2     4200
#define ZONE_END       5000
static bool s_lan;          // setup page open on the home network
static char s_lan_url[48];
static lv_obj_t *s_lan_panel;
static lv_obj_t *s_lan_msg;
#define INFO_ROWS      5
#define CARD_GEAR      96   // gear side of the setup card (the USB/button side)
static lv_obj_t *s_card_info;    // status list on the setup card
static lv_obj_t *s_info_dot[INFO_ROWS];
static lv_obj_t *s_info_txt[INFO_ROWS];
static lv_obj_t *s_night_panel;  // night mode: only a discreet clock, over everything
static lv_obj_t *s_night_time, *s_night_min, *s_night_date;
static bool s_night;             // night clock on screen
static bool s_night_on;          // setup page option
static int s_night_from = NIGHT_DEFAULT_FROM * 60, s_night_to = NIGHT_DEFAULT_TO * 60;   // minutes of the day
static int s_wake_left;          // seconds the panel stays awake at night (message, click)
static int s_night_msg_left;     // a new message at night: shown alone for a while, then back to the clock
static bool s_night_rem;         // a reminder at night: only the night clock and the reminder take turns
static bool s_rem_clock = true;  // ...which of the two is on screen
static int s_rem_left;
#define REM_CLOCK_S    5
#define REM_MSG_S      8
#define WAKE_CLICK_S   20
#define COLOR_NIGHT    lv_color_hex(0xc8823c)   // warm, dim amber
static lv_obj_t *s_wx_panel;     // weather details, right after the dashboard
static bool s_wx_page;           // on screen
static bool s_wx_on = true;      // setup page option
static int s_wx_s = CYCLE_DEFAULT_S;
static lv_obj_t *s_work_panel;   // "work" city weather, at its hours (after the weather details)
static bool s_work_page;
static work_wx_t s_work;
static lv_obj_t *s_cmp_panel;    // sensor comparisons, after the sensor pages
static int s_cmp = -1;           // comparison page on screen; -1 = none
static govee_cmp_t s_cmps[GOVEE_CMP_MAX];
static lv_obj_t *s_city_panel;   // other cities, after the dashboard
static lv_obj_t *s_city_time;
static lv_obj_t *s_city_ampm;
static lv_obj_t *s_ampm;          // AM/PM next to the main clock (12-hour setting)
static int s_city = -1;          // city page on screen; -1 = none
static city_wx_t s_cities[CITIES_MAX];
static int s_ncities;            // cities with data
static int s_city_s = CYCLE_DEFAULT_S;
static lv_obj_t *s_sens_panel;   // Govee sensors, between the dashboard and the messages
static int s_sens = -1;          // sensor page on screen; -1 = none
static govee_sensor_t s_govee[GOVEE_MAX];
static int s_ng;
static uint32_t s_govee_ver;

typedef struct {
    const char *text;
    uint32_t color;
} wmo_info_t;

int weather_kind(int code)
{
    return code == 0 ? WX_CLEAR : code <= 2 ? WX_PARTLY : code == 3 ? WX_CLOUDY : code <= 48 ? WX_FOG :
           code <= 57 ? WX_DRIZZLE : code <= 67 ? WX_RAIN : code <= 77 ? WX_SNOW : code <= 82 ? WX_SHOWERS :
           WX_THUNDER;
}

static wmo_info_t wmo_lookup(int code)
{
    // T->weather indexes: clear, partly cloudy, cloudy, fog, drizzle, rain, snow, showers, thunderstorm
    static const uint32_t COLORS[] = {0xffd54f, 0xffe082, 0x90a4ae, 0xb0bec5, 0x81d4fa,
                                      0x42a5f5, 0xffffff, 0x1e88e5, 0xab47bc};
    int i = weather_kind(code);
    return (wmo_info_t){T->weather[i], COLORS[i]};
}

// small static icon for the forecast rows (emoji size index: 0 = 16 px, 1 = 22 px)
static const lv_image_dsc_t *forecast_icon(int code, int size)
{
    static const uint32_t CP[] = {0x2600, 0x26C5, 0x2601, 0x1F32B, 0x1F326, 0x1F327, 0x1F328, 0x1F327, 0x26C8};
    return emoji_image(CP[weather_kind(code)], size);
}

static float temp_unit(float c)
{
    return s_fahrenheit ? c * 9.0f / 5.0f + 32.0f : c;
}

uint32_t ui_weather_color(int code)
{
    return wmo_lookup(code).color;
}

const char *ui_weather_text(int code)
{
    return wmo_lookup(code).text;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

// lv_label_set_text_fmt in LVGL 9.3 doesn't undo the "..." of a LONG_DOT label before replacing the
// text, and then writes the saved letters into the new (maybe shorter) text: heap corruption.
// lv_label_set_text does it right, so every formatted text goes through here.
static void label_fmt(lv_obj_t *l, const char *fmt, ...)
{
    char text[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    lv_label_set_text(l, text);
}

// Single line that scrolls when it doesn't fit (titles with a name we don't control)
static void scroll_line(lv_obj_t *l, int width)
{
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_size(l, width, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
}

// Single line cut with "..." (LONG_DOT only cuts with a fixed height; otherwise the text wraps)
static void one_line(lv_obj_t *l, int width)
{
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_size(l, width, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, COLOR_CARD, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 10, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    return c;
}

static lv_obj_t *dot(lv_obj_t *parent, int size, lv_color_t color)
{
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(d, size, size);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_bg_color(d, color, 0);
    return d;
}

static void render_forecast(void)
{
    lv_obj_clean(s_forecast);
    if (!s_has_weather) {
        lv_obj_t *l = label(s_forecast, &font_pt_12, COLOR_MUTED);
        lv_label_set_text(l, T->forecast_wait);
        return;
    }

    int inner_h = lv_obj_get_content_height(s_forecast);
    int row_h = inner_h / s_weather.n_days;

    // columns sized by the widest day name and temperature, with equal gaps between them
    char day_txt[FORECAST_DAYS][16], max_txt[FORECAST_DAYS][12], min_txt[FORECAST_DAYS][12];
    int day_w = 0, temp_w = 0;
    for (int i = 0; i < s_weather.n_days; i++) {
        const forecast_day_t *d = &s_weather.days[i];
        strlcpy(day_txt[i], i == 0 ? T->today : T->days[d->wday], sizeof(day_txt[i]));
        snprintf(max_txt[i], sizeof(max_txt[i]), "%.0f" DEG, temp_unit(d->temp_max));
        snprintf(min_txt[i], sizeof(min_txt[i]), "%.0f" DEG, temp_unit(d->temp_min));
        lv_point_t sz;
        lv_text_get_size(&sz, day_txt[i], &font_pt_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        day_w = LV_MAX(day_w, sz.x);
        lv_text_get_size(&sz, max_txt[i], &font_pt_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        temp_w = LV_MAX(temp_w, sz.x);
        lv_text_get_size(&sz, min_txt[i], &font_pt_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        temp_w = LV_MAX(temp_w, sz.x);
    }
    for (int i = 0; i < s_weather.n_days; i++) {
        const forecast_day_t *d = &s_weather.days[i];
        lv_obj_t *row = lv_obj_create(s_forecast);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), row_h);
        lv_obj_set_pos(row, 0, i * row_h);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *day = label(row, &font_pt_14, i == 0 ? COLOR_ACCENT : lv_color_white());
        lv_label_set_text(day, day_txt[i]);
        lv_obj_set_width(day, day_w);

        lv_obj_t *icon = lv_image_create(row);
        lv_image_set_src(icon, forecast_icon(d->code, row_h >= 26 ? 1 : 0));

        lv_obj_t *tmax = label(row, &font_pt_14, lv_color_white());
        lv_label_set_text(tmax, max_txt[i]);
        lv_obj_set_width(tmax, temp_w);
        lv_obj_set_style_text_align(tmax, LV_TEXT_ALIGN_RIGHT, 0);

        lv_obj_t *tmin = label(row, &font_pt_14, COLOR_MUTED);
        lv_label_set_text(tmin, min_txt[i]);
        lv_obj_set_width(tmin, temp_w);
        lv_obj_set_style_text_align(tmin, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

static void render_current(void)
{
    if (!s_has_weather) {
        return;
    }
    label_fmt(s_temp, "%.1f" DEG "%c", temp_unit(s_weather.temp), s_fahrenheit ? 'F' : 'C');
    weather_icon_set(s_wicon, weather_kind(s_weather.code), dimmer_is_day());
    if (s_extra) {
        label_fmt(s_extra, T->humidity_wind_fmt, s_weather.humidity, s_weather.wind_kmh);
    }
}

// Largest font in which the whole text fits an aw x ah area; NULL if not even the smallest fits
static const lv_font_t *fit_font(const char *text, int aw, int ah)
{
    // message fonts with the color emoji as fallback
    const lv_font_t *fonts[] = {emoji_font(&font_pt_28), emoji_font(&font_pt_20), emoji_font(&font_pt_14)};
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        lv_point_t sz;
        lv_text_get_size(&sz, text, fonts[i], 0, 0, aw, LV_TEXT_FLAG_NONE);
        if (sz.y <= ah) {
            return fonts[i];
        }
    }
    return NULL;
}

// Fits with the smallest font both upright and sideways (the screen may be rotated later)
static bool fits_any_rotation(const char *text)
{
    return fit_font(text, LCD_H_RES - 24, LCD_V_RES - MSG_TOP_TALL - MSG_BOTTOM) &&
           fit_font(text, LCD_V_RES - 24, LCD_H_RES - MSG_TOP - MSG_BOTTOM);
}

bool ui_text_fits(const char *text, int *max_chars)
{
    if (fits_any_rotation(text)) {
        return true;
    }
    if (max_chars) {
        // binary search on the number of (UTF-8) characters that still fit
        static char prefix[MSG_TEXT * 4];
        int total = 0;
        for (const char *p = text; *p; p++) {
            total += ((unsigned char)*p & 0xc0) != 0x80;
        }
        int lo = 0, hi = total;
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            // bytes of the first "mid" characters
            int bytes = 0, chars = 0;
            for (; text[bytes]; bytes++) {
                if (((unsigned char)text[bytes] & 0xc0) != 0x80 && chars++ == mid) {
                    break;
                }
            }
            if (bytes >= (int)sizeof(prefix)) {
                bytes = sizeof(prefix) - 1;
            }
            memcpy(prefix, text, bytes);
            prefix[bytes] = '\0';
            if (fits_any_rotation(prefix)) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        *max_chars = lo;
    }
    return false;
}

// "09:15" today, "15/10 09:15" on another day
static void fmt_time(char *out, size_t size, time_t t)
{
    struct tm tm, today;
    time_t now = time(NULL);
    localtime_r(&t, &tm);
    localtime_r(&now, &today);
    char hm[12];
    clock_fmt(hm, sizeof(hm), tm.tm_hour, tm.tm_min);
    if (tm.tm_yday == today.tm_yday && tm.tm_year == today.tm_year) {
        snprintf(out, size, "%s", hm);
    } else {
        snprintf(out, size, "%02d/%02d %s", tm.tm_mday, tm.tm_mon + 1, hm);
    }
}

static void info_row(int i, uint32_t color, const char *fmt, ...)
{
    char text[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    lv_obj_set_style_bg_color(s_info_dot[i], lv_color_hex(color), 0);
    // only when it changes: setting the text restarts the scrolling of long lines
    if (strcmp(lv_label_get_text(s_info_txt[i]), text) != 0) {
        lv_label_set_text(s_info_txt[i], text);
    }
}

// Status list on the setup card: is everything connected?
static void update_card_info(void)
{
    if (!s_card_info || lv_obj_has_flag(s_card_info, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    const uint32_t OK = 0x22c55e, WAIT = 0xffb74d, BAD = 0xff4d4f, OFF = 0x8a94a8;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        esp_netif_ip_info_t ip = {0};
        esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
        info_row(0, OK, "IP " IPSTR, IP2STR(&ip.ip));   // signal strength is already on the dashboard
    } else {
        info_row(0, BAD, "%s", T->info_wifi_off);
    }

    if (s_has_weather) {
        char when[40];
        fmt_time(when, sizeof(when), s_weather.updated_at);
        info_row(1, time(NULL) - s_weather.updated_at < 40 * 60 ? OK : WAIT, T->info_weather_fmt, when);
    } else {
        info_row(1, WAIT, "%s", T->info_weather_wait);
    }

    switch (telegram_state()) {
    case TG_OK: info_row(2, OK, "%s", T->info_tg_ok); break;
    case TG_FAILING: info_row(2, BAD, "%s", T->info_tg_fail); break;
    case TG_CONNECTING: info_row(2, WAIT, "%s", T->info_tg_wait); break;
    default: info_row(2, OFF, "%s", T->info_tg_off); break;
    }

    govee_status_t gv = govee_status();
    if (!gv.on) {
        info_row(3, OFF, "%s", T->info_gv_off);
    } else if (gv.http == 401 || gv.http == 403) {
        info_row(3, BAD, "%s", T->info_gv_key);
    } else if (gv.http == 0) {
        info_row(3, WAIT, "%s", T->info_gv_wait);
    } else if (gv.http != 200) {
        info_row(3, BAD, T->info_gv_err_fmt, gv.http);
    } else {
        // connected; orange while some sensor is off the screen (waiting for its first change, or gone quiet)
        info_row(3, gv.shown == gv.sensors ? OK : WAIT, T->info_gv_ok_fmt, gv.shown, gv.sensors);
    }

    int64_t up = esp_timer_get_time() / 1000000;
    char span[24];
    if (up >= 86400) {
        snprintf(span, sizeof(span), "%dd %dh", (int)(up / 86400), (int)(up % 86400 / 3600));
    } else {
        snprintf(span, sizeof(span), "%dh%02dm", (int)(up / 3600), (int)(up % 3600 / 60));
    }
    info_row(4, OFF, T->info_up_fmt, span);
}

static bool s_night_effective;   // night mode is on now (by the hours or forced from the card)
static bool s_force_night;       // turned on from the card
static bool s_skip_night;        // turned off from the card for the rest of tonight's hours
static time_t s_force_since;
#define FORCE_MAX_S    (12 * 3600)   // a forced night ends by itself after 12 h
static void render_night_card(int aw)
{
    label_fmt(s_msg_title, LV_SYMBOL_EYE_CLOSE "  %s", T->night_card_title);
    lv_obj_set_style_text_color(s_msg_title, COLOR_NIGHT, 0);
    lv_label_set_text(s_msg_when, "");
    lv_obj_add_flag(s_card_info, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(s_card_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_card_img, LV_ALIGN_CENTER, 0, (s_msg_top - MSG_BOTTOM) / 2 - 22);
    lv_obj_set_style_transform_scale(s_msg_label, 256, 0);
    lv_obj_set_style_text_font(s_msg_label, &font_pt_20, 0);
    lv_label_set_long_mode(s_msg_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(s_msg_label, aw, LV_SIZE_CONTENT);
    lv_label_set_text(s_msg_label, s_night_effective ? T->night_card_on : T->night_card_off);
    lv_obj_set_style_text_color(s_msg_label, s_night_effective ? COLOR_NIGHT : lv_color_white(), 0);
    lv_obj_align(s_msg_label, LV_ALIGN_CENTER, 0, (s_msg_top - MSG_BOTTOM) / 2 + 24);
    lv_label_set_text(s_msg_hint, T->night_card_hint);
    lv_obj_remove_flag(s_msg_hint, LV_OBJ_FLAG_HIDDEN);
}

// "18:00 · 30 min · every day" (or "25/10 18:00 · 1 h · once")
static void sched_line(const sched_t *sc, char *out, size_t size)
{
    char when[24], dur[16];
    if (sc->repeat == REPEAT_ONCE) {
        time_t t = (time_t)sc->start;
        struct tm tm;
        localtime_r(&t, &tm);
        char hm[12];
        clock_fmt(hm, sizeof(hm), tm.tm_hour, tm.tm_min);
        snprintf(when, sizeof(when), "%02d/%02d %s", T->month_first ? tm.tm_mon + 1 : tm.tm_mday,
                 T->month_first ? tm.tm_mday : tm.tm_mon + 1, hm);
    } else {
        clock_fmt(when, sizeof(when), sc->hour, sc->minute);
    }
    int m = sc->dur_min;
    if (m < 60) {
        snprintf(dur, sizeof(dur), "%d min", m);
    } else if (m < 1440) {
        snprintf(dur, sizeof(dur), m % 60 ? "%dh%02d" : "%d h", m / 60, m % 60);
    } else {
        snprintf(dur, sizeof(dur), "%d d", m / 1440);
    }
    snprintf(out, size, "%s \xC2\xB7 %s \xC2\xB7 %s", when, dur, T->repeat[sc->repeat % 4]);
}

static void render_sched_card(int aw)
{
    label_fmt(s_msg_title, LV_SYMBOL_BELL "  %s", T->sv_card_title);
    lv_obj_set_style_text_color(s_msg_title, COLOR_ACCENT, 0);
    lv_label_set_text(s_msg_when, "");
    lv_obj_add_flag(s_card_info, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_card_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_transform_scale(s_msg_label, 256, 0);
    lv_obj_set_style_text_font(s_msg_label, &font_pt_20, 0);
    lv_label_set_long_mode(s_msg_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(s_msg_label, aw, LV_SIZE_CONTENT);
    if (s_sv_n == 0) {
        lv_label_set_text(s_msg_label, T->sv_card_empty);
    } else if (s_sv_n == 1) {
        lv_label_set_text(s_msg_label, T->sv_count_one);
    } else {
        label_fmt(s_msg_label, T->sv_count_fmt, s_sv_n);
    }
    lv_obj_set_style_text_color(s_msg_label, lv_color_white(), 0);
    lv_obj_align(s_msg_label, LV_ALIGN_CENTER, 0, (s_msg_top - MSG_BOTTOM) / 2);
    lv_label_set_text(s_msg_hint, s_sv_n ? T->sv_card_hint : "");
    lv_obj_remove_flag(s_msg_hint, LV_OBJ_FLAG_HIDDEN);
}

// One schedule: its text in the largest font that fits, its time/duration/repeat and state below
static void render_sched_view(int aw, int ah)
{
    if (s_sv_idx >= s_sv_n) {
        s_sv_idx = s_sv_n - 1;
    }
    const sched_t *sc = &s_sv_list[s_sv_idx];
    char title[48];
    snprintf(title, sizeof(title), T->sv_title_fmt, s_sv_idx + 1, s_sv_n);
    label_fmt(s_msg_title, LV_SYMBOL_BELL "  %s", title);
    lv_obj_set_style_text_color(s_msg_title, COLOR_ACCENT, 0);
    lv_label_set_text(s_msg_when, "");
    lv_obj_add_flag(s_card_info, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_card_img, LV_OBJ_FLAG_HIDDEN);

    char line[96];
    sched_line(sc, line, sizeof(line));
    int st = sched_state(sc->id);
    const char *tag = st == SCHED_ON_SCREEN ? T->sch_on_now : st == SCHED_DISMISSED ? T->sch_dismissed :
                      st == SCHED_SKIPPED ? T->sch_skipped : NULL;
    if (tag) {
        size_t l = strlen(line);
        snprintf(line + l, sizeof(line) - l, "\n%s", tag);
    }
    lv_label_set_text(s_sv_info, line);
    // the first line always at the same height, with or without the state line under it
    int vh = lv_display_get_vertical_resolution(NULL);
    lv_obj_align(s_sv_info, LV_ALIGN_TOP_MID, 0, vh - MSG_BOTTOM - 2 - 2 * lv_font_get_line_height(&font_pt_12));
    lv_obj_set_style_text_color(s_sv_info, st == SCHED_ON_SCREEN ? COLOR_ACCENT : COLOR_MUTED, 0);
    lv_obj_remove_flag(s_sv_info, LV_OBJ_FLAG_HIDDEN);

    int th = ah - 34;   // room for the two info lines
    // big font, or the medium one; never the small one here: it rolls slowly instead
    const lv_font_t *f28 = emoji_font(&font_pt_28), *f20 = emoji_font(&font_pt_20);
    lv_point_t sz;
    lv_text_get_size(&sz, sc->text, f28, 0, 0, aw, LV_TEXT_FLAG_NONE);
    const lv_font_t *font = sz.y <= th ? f28 : f20;
    lv_text_get_size(&sz, sc->text, f20, 0, 0, aw, LV_TEXT_FLAG_NONE);
    bool roll = font == f20 && sz.y > th;
    s_sv_roll = roll;
    lv_obj_set_style_transform_scale(s_msg_label, 256, 0);
    lv_obj_set_style_text_color(s_msg_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_msg_label, font, 0);
    lv_label_set_long_mode(s_msg_label, roll ? LV_LABEL_LONG_MODE_SCROLL : LV_LABEL_LONG_WRAP);
    if (roll) {
        // LVGL's scroll mode makes the label one long line rolling sideways; keep the line breaks so
        // it rolls up and down instead (much easier to read)
        ((lv_label_t *)s_msg_label)->expand = 0;
    }
    lv_obj_set_style_anim_duration(s_msg_label, roll ? 12000 : 0, 0);
    lv_obj_set_size(s_msg_label, aw, roll ? th : LV_SIZE_CONTENT);
    // time to read it: 20 s plus 1 s per 4 characters, up to two minutes
    s_browse_left = LV_MIN(120, 20 + (int)strlen(sc->text) / 4);
    lv_label_set_text(s_msg_label, sc->text);
    lv_obj_align(s_msg_label, LV_ALIGN_CENTER, 0, (s_msg_top - MSG_BOTTOM - 34) / 2);
    lv_label_set_text(s_msg_hint, T->sv_hint);
    lv_obj_remove_flag(s_msg_hint, LV_OBJ_FLAG_HIDDEN);
}

static void render_message(void)
{
    if (!s_msg_panel) {
        return;
    }
    int w = lv_display_get_horizontal_resolution(NULL);
    int h = lv_display_get_vertical_resolution(NULL);
    int aw = w - 24, ah = h - s_msg_top - MSG_BOTTOM;
    lv_obj_add_flag(s_sv_info, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_zone, LV_OBJ_FLAG_HIDDEN);
    if (s_card && s_sv) {
        render_sched_view(aw, ah);
        return;
    }
    if (s_card && s_card_sched) {
        render_sched_card(aw);
        return;
    }
    if (s_card && s_card_night) {
        render_night_card(aw);
        return;
    }
    lv_obj_add_flag(s_card_img, LV_OBJ_FLAG_HIDDEN);
    if (s_card) {
        // last stop of the browse sequence: open the setup page on the home network
        label_fmt(s_msg_title, LV_SYMBOL_SETTINGS "  %s", T->card_title);
        lv_obj_set_style_text_color(s_msg_title, COLOR_ACCENT, 0);
        lv_label_set_text(s_msg_when, "");

        // split: a big gear on the side of the USB edge (where the button is), status on the other
        int ch = h - s_msg_top - MSG_BOTTOM;
        int usb = 0;   // 0 bottom, 1 left, 2 top, 3 right
        for (int i = 0; i < 4; i++) {
            if (BOARD_USB_TO_ROT[i] == lv_display_get_rotation(NULL)) {
                usb = i;
            }
        }
        bool landscape = w > h;
        bool gear_first = landscape ? usb != 3 : usb == 2;   // left/top part
        int gx, gy, gw, gh, ix, iy, iw, ih;
        if (landscape) {
            gw = CARD_GEAR, gh = ch, iw = w - CARD_GEAR - 16, ih = ch;
            gx = gear_first ? 0 : w - CARD_GEAR, ix = gear_first ? CARD_GEAR : 8;
            gy = iy = s_msg_top;
        } else {
            gw = w, gh = CARD_GEAR, iw = w - 20, ih = ch - CARD_GEAR;
            gy = gear_first ? s_msg_top : h - MSG_BOTTOM - CARD_GEAR, iy = gear_first ? s_msg_top + CARD_GEAR : s_msg_top + 4;
            gx = 0, ix = 10;
        }
        lv_obj_set_style_text_font(s_msg_label, &font_pt_28, 0);
        lv_obj_set_style_transform_scale(s_msg_label, 512, 0);   // 2x
        lv_obj_set_style_transform_pivot_x(s_msg_label, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(s_msg_label, lv_pct(50), 0);
        lv_label_set_long_mode(s_msg_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_size(s_msg_label, gw, LV_SIZE_CONTENT);
        lv_label_set_text(s_msg_label, LV_SYMBOL_SETTINGS);
        lv_obj_set_style_text_color(s_msg_label, COLOR_ACCENT, 0);
        lv_obj_update_layout(s_msg_label);
        // align, not set_pos: a message before left the label centered, and set_pos would be relative to that
        lv_obj_align(s_msg_label, LV_ALIGN_TOP_LEFT, gx, gy + (gh - lv_obj_get_height(s_msg_label)) / 2);

        lv_obj_set_pos(s_card_info, ix, iy);
        lv_obj_set_size(s_card_info, iw, ih);
        lv_obj_remove_flag(s_card_info, LV_OBJ_FLAG_HIDDEN);
        update_card_info();

        lv_label_set_text(s_msg_hint, T->card_hint);
        lv_obj_remove_flag(s_msg_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_card_info, LV_OBJ_FLAG_HIDDEN);
    if (s_idx < 0 || s_idx >= s_n) {
        lv_obj_add_flag(s_msg_panel, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const msg_item_t *m = &s_items[s_idx];
    lv_label_set_text(s_msg_hint, T->msg_browse_hint);

    char count[32] = "";
    if (s_n > 1) {
        snprintf(count, sizeof(count), "  %d/%d", s_idx + 1, s_n);
    }
    // unread (not opened with the button yet): orange header with a "new" tag
    label_fmt(s_msg_title, "%s  %s%s%s%s", m->scheduled ? LV_SYMBOL_BELL : LV_SYMBOL_ENVELOPE,
                          T->msg_title, count, m->unread ? "  \xC2\xB7  " : "", m->unread ? T->msg_unread_tag : "");
    lv_obj_set_style_text_color(s_msg_title, m->unread ? COLOR_UNREAD : COLOR_ACCENT, 0);

    const lv_font_t *font = fit_font(m->text, aw, ah);
    lv_obj_set_style_transform_scale(s_msg_label, 256, 0);
    lv_obj_set_style_text_color(s_msg_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_msg_label, font ? font : emoji_font(&font_pt_14), 0);
    lv_label_set_long_mode(s_msg_label, font ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_msg_label, aw, font ? LV_SIZE_CONTENT : ah);
    lv_label_set_text(s_msg_label, m->text);
    lv_obj_align(s_msg_label, LV_ALIGN_CENTER, 0, (s_msg_top - MSG_BOTTOM) / 2);

    if (m->when > 0) {
        struct tm t, today;
        time_t now = time(NULL);
        localtime_r(&m->when, &t);
        localtime_r(&now, &today);
        char hm[12], when[48];
        clock_fmt(hm, sizeof(hm), t.tm_hour, t.tm_min);
        if (t.tm_yday == today.tm_yday && t.tm_year == today.tm_year) {
            snprintf(when, sizeof(when), T->msg_today_fmt, hm);
        } else {
            snprintf(when, sizeof(when), "%02d/%02d %s", t.tm_mday, t.tm_mon + 1, hm);
        }
        // with more than one person on the bot: "Ana · today 14:05"
        label_fmt(s_msg_when, "%s%s%s", m->from, m->from[0] ? " \xC2\xB7 " : "", when);
    } else {
        lv_label_set_text(s_msg_when, "");
    }

    if (s_browse) {
        lv_obj_remove_flag(s_msg_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_msg_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

// Pages over the dashboard (top layer): messages/setup card, sensor pages, other cities
enum { OV_NONE, OV_MSG, OV_SENS, OV_CITY, OV_WX, OV_CMP, OV_WORK };

static int overlay_now(void)
{
    return s_idx >= 0 || s_card ? OV_MSG : s_sens >= 0 ? OV_SENS : s_city >= 0 ? OV_CITY : s_wx_page ? OV_WX :
           s_cmp >= 0 ? OV_CMP : s_work_page ? OV_WORK : OV_NONE;
}

static lv_obj_t *overlay_obj(int ov)
{
    return ov == OV_MSG ? s_msg_panel : ov == OV_SENS ? s_sens_panel : ov == OV_CITY ? s_city_panel :
           ov == OV_WX ? s_wx_panel : ov == OV_CMP ? s_cmp_panel : ov == OV_WORK ? s_work_panel : NULL;
}

// Shows overlay "next" (already rendered) after "prev": fades over the dashboard, cuts between pages
static void present(int prev, int next)
{
    lv_obj_t *po = overlay_obj(prev), *no = overlay_obj(next);
    if (!no) {
        if (po) {
            lv_obj_fade_out(po, 300, 0);
        }
        return;
    }
    for (int ov = OV_MSG; ov <= OV_WORK; ov++) {
        if (ov != next && overlay_obj(ov)) {
            lv_obj_add_flag(overlay_obj(ov), LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_remove_flag(no, LV_OBJ_FLAG_HIDDEN);
    if (prev != OV_NONE) {
        lv_obj_set_style_opa(no, LV_OPA_COVER, 0);
    } else {
        lv_obj_fade_in(no, 300, 0);
    }
}

// Switches what's on screen: dashboard (idx -1) or message idx
static void show(int idx)
{
    int prev = overlay_now();
    s_card = false;
    s_card_night = false;
    s_card_sched = false;
    s_sv = false;
    s_idx = idx;
    s_sens = -1;
    s_city = -1;
    s_cmp = -1;
    s_work_page = false;
    s_wx_page = false;
    if (!s_msg_panel) {
        return;
    }
    if (idx < 0) {
        lv_obj_add_flag(s_msg_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        render_message();
    }
    present(prev, idx < 0 ? OV_NONE : OV_MSG);
}

static void refresh_items(void)
{
    s_n = msg_active(s_items, MSG_ITEMS_MAX);
}

// Showing a message in browse mode = the user opened it on purpose: mark it read
static void browse_show(int idx)
{
    if (idx >= 0 && idx < s_n && s_items[idx].unread) {
        msg_mark_read(s_items[idx].key);
        s_items[idx].unread = false;
    }
    show(idx);
}

static void show_card(void)
{
    int prev = overlay_now();
    s_idx = -1;
    s_card = true;
    s_sens = -1;
    s_city = -1;
    s_cmp = -1;
    s_work_page = false;
    s_wx_page = false;
    if (!s_msg_panel) {
        return;
    }
    render_message();
    present(prev, OV_MSG);
}

static int sens_pages(void)
{
    return (s_ng + SENS_PER_PAGE - 1) / SENS_PER_PAGE;
}

/*
 * One card per sensor, two per page. Portrait: one above the other; landscape: side by side.
 * +----------------------+
 * | Living room          |
 * | 22.9°C  ^            |   ^ v -  trend over the last hour
 * | min 21.3° max 24.1°  |
 * | 61% hum              |
 * | updated 09:15        |
 * +----------------------+
 */
static void render_sensors(void)
{
    if (!s_sens_panel || s_sens < 0) {
        return;
    }
    lv_obj_clean(s_sens_panel);
    int w = lv_display_get_horizontal_resolution(NULL);
    int h = lv_display_get_vertical_resolution(NULL);
    bool landscape = w > h;

    lv_obj_t *title = label(s_sens_panel, &font_pt_14, COLOR_ACCENT);
    if (sens_pages() > 1) {
        label_fmt(title, LV_SYMBOL_HOME "  %s  %d/%d", T->sens_title, s_sens + 1, sens_pages());
    } else {
        label_fmt(title, LV_SYMBOL_HOME "  %s", T->sens_title);
    }
    lv_obj_set_pos(title, 10, 8);
    lv_obj_t *bar = lv_obj_create(s_sens_panel);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, w - 20, 2);
    lv_obj_set_pos(bar, 10, 30);
    lv_obj_set_style_bg_color(bar, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);

    int avail = h - MSG_TOP - 6;
    int on_page = s_ng - s_sens * SENS_PER_PAGE;
    on_page = on_page > SENS_PER_PAGE ? SENS_PER_PAGE : on_page;
    bool single = on_page == 1;   // alone on the page: one big card on the whole screen
    int cw = landscape && !single ? (w - 18) / 2 : w - 12;
    int ch = landscape || single ? avail : (avail - 6) / 2;
    char unit = s_fahrenheit ? 'F' : 'C';
    for (int k = 0; k < on_page; k++) {
        const govee_sensor_t *g = &s_govee[s_sens * SENS_PER_PAGE + k];
        lv_obj_t *c = card(s_sens_panel, landscape ? 6 + k * (cw + 6) : 6, landscape ? MSG_TOP : MSG_TOP + k * (ch + 6),
                           cw, ch);
        // long names roll instead of wrapping under the temperature
        lv_obj_t *name = label(c, &font_pt_14, lv_color_white());
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
        lv_obj_set_width(name, cw - 16);
        lv_label_set_text(name, g->name);

        lv_obj_t *foot = label(c, &font_pt_12, COLOR_MUTED);
        lv_obj_align(foot, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        if (g->changed_at) {
            char when[40];
            fmt_time(when, sizeof(when), g->changed_at);
            label_fmt(foot, T->sens_updated_fmt, when);
        } else {
            lv_label_set_text(foot, T->sens_wait);   // a reading, but no upload seen yet
        }

        // single: 48 px temperature; portrait stacks everything, landscape puts humidity on the right
        int y_temp = single ? (landscape ? 20 : 44) : 18;
        int y_range = single ? (landscape ? 76 : 108) : 52;
        lv_obj_t *temp = label(c, single ? &lv_font_montserrat_48 : &font_pt_28, lv_color_white());
        label_fmt(temp, "%.1f" DEG "%c", temp_unit(g->temp_c), unit);
        lv_obj_set_pos(temp, 0, y_temp);
        if (g->has_trend) {
            lv_obj_t *arrow = label(c, single ? &font_pt_28 : &font_pt_20, g->trend > 0 ? lv_color_hex(0xff8a65) :
                                                                         g->trend < 0 ? COLOR_ACCENT : COLOR_MUTED);
            lv_label_set_text(arrow, g->trend > 0 ? LV_SYMBOL_UP : g->trend < 0 ? LV_SYMBOL_DOWN : LV_SYMBOL_MINUS);
            lv_obj_update_layout(temp);
            lv_obj_align_to(arrow, temp, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
        }

        lv_obj_t *range = label(c, single ? &font_pt_14 : &font_pt_12, COLOR_MUTED);
        label_fmt(range, T->sens_minmax_fmt, temp_unit(g->day_min), temp_unit(g->day_max));
        lv_obj_set_pos(range, 0, y_range);

        if (g->humidity >= 0) {
            lv_obj_t *hum = label(c, single ? &font_pt_20 : &font_pt_14, lv_color_white());
            label_fmt(hum, T->sens_hum_fmt, g->humidity);
            if (single && landscape) {
                lv_obj_align(hum, LV_ALIGN_TOP_RIGHT, 0, 34);
            } else {
                lv_obj_set_pos(hum, 0, single ? 136 : 70);
            }
        }
    }
}

static void show_sensors(int page)
{
    int prev = overlay_now();
    s_idx = -1;
    s_card = false;
    s_card_night = false;
    s_card_sched = false;
    s_sv = false;
    s_sens = page;
    s_city = -1;
    s_cmp = -1;
    s_work_page = false;
    s_wx_page = false;
    if (!s_sens_panel) {
        return;
    }
    render_sensors();
    present(prev, OV_SENS);
}

static void build_sensor_panel(int w, int h)
{
    s_sens_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_sens_panel);
    lv_obj_set_size(s_sens_panel, w, h);
    lv_obj_set_style_bg_color(s_sens_panel, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_sens_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_sens_panel, LV_OBJ_FLAG_SCROLLABLE);
    if (s_sens >= sens_pages()) {
        s_sens = -1;
    }
    if (s_sens >= 0) {
        render_sensors();
    } else {
        lv_obj_add_flag(s_sens_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void exit_browse(void)
{
    s_browse = false;
    s_left = s_dash_s;
    show(-1);
}

// Message screen: lives on the top layer and alternates with the dashboard
static void build_message_panel(int w, int h)
{
    s_msg_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_msg_panel);
    lv_obj_set_size(s_msg_panel, w, h);
    lv_obj_set_style_bg_color(s_msg_panel, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_msg_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_msg_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_msg_panel, LV_OBJ_FLAG_HIDDEN);

    // landscape: title and date/time on one row; portrait: the date/time on a line of its own.
    // A title that doesn't fit ("Message 2/3 · new") scrolls.
    bool tall = h > w;
    s_msg_top = tall ? MSG_TOP_TALL : MSG_TOP;
    s_msg_title = label(s_msg_panel, &font_pt_14, COLOR_ACCENT);
    scroll_line(s_msg_title, tall ? w - 20 : w - 100);
    lv_obj_set_pos(s_msg_title, 10, 8);
    s_msg_when = label(s_msg_panel, &font_pt_12, COLOR_MUTED);
    if (tall) {
        lv_obj_set_pos(s_msg_when, 10, 28);
    } else {
        lv_obj_align(s_msg_when, LV_ALIGN_TOP_RIGHT, -10, 10);
    }

    lv_obj_t *bar = lv_obj_create(s_msg_panel);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, w - 20, 2);
    lv_obj_set_pos(bar, 10, s_msg_top - 6);
    lv_obj_set_style_bg_color(bar, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);

    s_card_img = lv_image_create(s_msg_panel);
    lv_image_set_src(s_card_img, &EMOJI_BIG[EMOJI_BIG_MOON]);
    lv_obj_add_flag(s_card_img, LV_OBJ_FLAG_HIDDEN);
    s_msg_label = label(s_msg_panel, &font_pt_20, lv_color_white());
    lv_obj_set_style_text_align(s_msg_label, LV_TEXT_ALIGN_CENTER, 0);

    // status list of the setup card (placed by render_message)
    s_card_info = lv_obj_create(s_msg_panel);
    lv_obj_remove_style_all(s_card_info);
    lv_obj_remove_flag(s_card_info, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_card_info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_card_info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_card_info, 2, 0);
    for (int i = 0; i < INFO_ROWS; i++) {
        lv_obj_t *row = lv_obj_create(s_card_info);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 20);
        s_info_dot[i] = dot(row, 9, COLOR_MUTED);
        lv_obj_align(s_info_dot[i], LV_ALIGN_LEFT_MID, 0, 0);
        s_info_txt[i] = label(row, &font_pt_14, lv_color_white());
        // long lines (portrait, Spanish...) scroll instead of ending in "..."
        lv_label_set_long_mode(s_info_txt[i], LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
        lv_obj_set_size(s_info_txt[i], lv_pct(90), lv_font_get_line_height(&font_pt_14));
        lv_obj_align(s_info_txt[i], LV_ALIGN_LEFT_MID, 14, 0);
    }
    lv_obj_add_flag(s_card_info, LV_OBJ_FLAG_HIDDEN);

    s_msg_hint = label(s_msg_panel, &font_pt_12, COLOR_MUTED);
    lv_label_set_text(s_msg_hint, T->msg_browse_hint);
    lv_obj_align(s_msg_hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_add_flag(s_msg_hint, LV_OBJ_FLAG_HIDDEN);

    // red bar that fills while the button is held
    s_del_bar = lv_bar_create(s_msg_panel);
    lv_obj_set_size(s_del_bar, w - 20, 8);
    lv_obj_align(s_del_bar, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_bar_set_range(s_del_bar, 0, 100);
    lv_obj_set_style_bg_color(s_del_bar, lv_color_hex(0x3a1d24), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_del_bar, lv_color_hex(0xff4d4f), LV_PART_INDICATOR);
    lv_obj_add_flag(s_del_bar, LV_OBJ_FLAG_HIDDEN);

    // "deleted" notice: big trash can over the message
    s_del_done = lv_obj_create(s_msg_panel);
    lv_obj_remove_style_all(s_del_done);
    lv_obj_set_size(s_del_done, w, h - s_msg_top);
    lv_obj_set_pos(s_del_done, 0, s_msg_top);
    lv_obj_set_style_bg_color(s_del_done, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_del_done, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_del_done, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_del_done, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *icon = label(s_del_done, &font_pt_28, lv_color_hex(0xff4d4f));
    lv_label_set_text(icon, LV_SYMBOL_TRASH);
    s_del_txt = label(s_del_done, &font_pt_20, lv_color_hex(0xff4d4f));
    lv_label_set_text(s_del_txt, T->msg_deleted);
    lv_obj_add_flag(s_del_done, LV_OBJ_FLAG_HIDDEN);

    // schedule browser: info line and the hold bar with its four zones
    // two fixed lines (time · duration · repeat, then the state): nothing rolls away while you read
    s_sv_info = label(s_msg_panel, &font_pt_12, COLOR_MUTED);
    lv_label_set_long_mode(s_sv_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_sv_info, w - 20);
    lv_obj_set_style_text_align(s_sv_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_sv_info, LV_ALIGN_BOTTOM_MID, 0, -(MSG_BOTTOM + 2));
    lv_obj_add_flag(s_sv_info, LV_OBJ_FLAG_HIDDEN);
    s_zone = lv_obj_create(s_msg_panel);
    lv_obj_remove_style_all(s_zone);
    lv_obj_set_size(s_zone, w - 20, 10);
    lv_obj_align(s_zone, LV_ALIGN_BOTTOM_MID, 0, -6);
    static const uint32_t ZCOL[] = {0x5a6478, 0xff9800, 0xff4d4f, 0x5a6478};
    const int zt[] = {ZONE_START, ZONE_TODAY, ZONE_ALL, ZONE_NONE2, ZONE_END};
    int zw = w - 20;
    for (int z = 0; z < 4; z++) {
        lv_obj_t *seg = lv_obj_create(s_zone);
        lv_obj_remove_style_all(seg);
        int x0 = (zt[z] - ZONE_START) * zw / (ZONE_END - ZONE_START);
        int x1 = (zt[z + 1] - ZONE_START) * zw / (ZONE_END - ZONE_START);
        lv_obj_set_size(seg, x1 - x0 - 2, 10);
        lv_obj_set_pos(seg, x0 + 1, 0);
        lv_obj_set_style_radius(seg, 3, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(ZCOL[z]), 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_60, 0);
    }
    s_zone_mark = lv_obj_create(s_zone);
    lv_obj_remove_style_all(s_zone_mark);
    lv_obj_set_size(s_zone_mark, 4, 10);
    lv_obj_set_style_bg_color(s_zone_mark, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_zone_mark, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_zone_mark, 2, 0);
    lv_obj_add_flag(s_zone, LV_OBJ_FLAG_HIDDEN);
    s_zone_lbl = label(s_msg_panel, &font_pt_14, lv_color_white());
    lv_obj_add_flag(s_zone_lbl, LV_OBJ_FLAG_HIDDEN);

    if (s_idx >= s_n) {
        s_idx = -1;
    }
    if (s_idx >= 0 || s_card) {
        render_message();
        lv_obj_remove_flag(s_msg_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------- weather details ----------

static bool wx_available(void)
{
    return s_wx_on && s_has_weather;
}

// Widget-like tile: name on top, big value, level below and a thin level bar at the bottom.
// Landscape: tall tiles side by side; portrait: wide tiles stacked (value on the right).
static void wx_tile(lv_obj_t *parent, int x, int y, int w, int h, bool wide, const char *name, const char *value,
                    uint32_t value_color, const char *level, uint32_t level_color, int frac_pct)
{
    lv_obj_t *t = card(parent, x, y, w, h);
    lv_obj_set_style_pad_all(t, 6, 0);
    int iw = w - 12;
    lv_obj_t *n = label(t, &font_pt_12, COLOR_MUTED);
    one_line(n, wide ? iw / 2 + 10 : iw);
    lv_label_set_text(n, name);
    lv_obj_set_pos(n, 0, 0);
    lv_obj_t *v = label(t, &font_pt_28, lv_color_hex(value_color));
    lv_label_set_text(v, value);
    lv_obj_t *l = label(t, &font_pt_12, lv_color_hex(level_color));
    scroll_line(l, wide ? iw / 2 + 20 : iw);
    lv_label_set_text(l, level);
    if (wide) {
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, -2);
        lv_obj_set_pos(l, 0, h - 12 - 22);
    } else {
        lv_obj_set_pos(v, 0, 10);
        lv_obj_set_pos(l, 0, h - 12 - 18);
    }
    if (frac_pct >= 0) {   // where the value sits on its scale
        lv_obj_t *track = lv_obj_create(t);
        lv_obj_remove_style_all(track);
        lv_obj_set_size(track, iw, 3);
        lv_obj_align(track, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_radius(track, 2, 0);
        lv_obj_set_style_bg_color(track, lv_color_hex(0x2a3348), 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_t *fill = lv_obj_create(track);
        lv_obj_remove_style_all(fill);
        lv_obj_set_size(fill, LV_MAX(3, iw * LV_MIN(frac_pct, 100) / 100), 3);
        lv_obj_set_style_radius(fill, 2, 0);
        lv_obj_set_style_bg_color(fill, lv_color_hex(level_color), 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    }
}

/*
 * Landscape: three tiles (feels like, UV, air) across the top, the rain chart across the bottom.
 * Portrait: the three tiles stacked, the rain chart below.
 */
static void render_wx(void)
{
    if (!s_wx_panel || !s_wx_page || !s_has_weather) {
        return;
    }
    lv_obj_clean(s_wx_panel);
    const weather_t *w = &s_weather;
    int sw = lv_display_get_horizontal_resolution(NULL);
    int sh = lv_display_get_vertical_resolution(NULL);
    bool landscape = sw > sh;

    lv_obj_t *title = label(s_wx_panel, &font_pt_14, COLOR_ACCENT);
    scroll_line(title, sw - 20);
    label_fmt(title, LV_SYMBOL_GPS "  %s \xC2\xB7 %s", T->wx_title, g_cfg.city);
    lv_obj_set_pos(title, 10, 8);
    lv_obj_t *bar = lv_obj_create(s_wx_panel);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, sw - 20, 2);
    lv_obj_set_pos(bar, 10, 30);
    lv_obj_set_style_bg_color(bar, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);

    // UV (WHO scale) and European AQI bands with their usual colors
    static const uint32_t UV_COLOR[] = {0x4caf50, 0xfdd835, 0xff9800, 0xf44336, 0xab47bc};
    static const uint32_t AQI_COLOR[] = {0x50f0e6, 0x50ccaa, 0xf0e641, 0xff5050, 0xd04070, 0xab47bc};
    bool night_uv = w->uv <= 0.4f;   // at night "now" is always 0: show today's max
    float uv = night_uv ? w->uv_max : w->uv;
    int uvi = (int)(uv + 0.5f);
    int uvl = uvi <= 2 ? 0 : uvi <= 5 ? 1 : uvi <= 7 ? 2 : uvi <= 10 ? 3 : 4;
    int aql = w->aqi < 0 ? -1 : w->aqi <= 20 ? 0 : w->aqi <= 40 ? 1 : w->aqi <= 60 ? 2 : w->aqi <= 80 ? 3 :
              w->aqi <= 100 ? 4 : 5;

    // tiles
    int n_tiles = aql >= 0 ? 3 : 2, gap = 6;
    int tw = landscape ? (sw - 20 - gap * (n_tiles - 1)) / n_tiles : sw - 20;
    int th = landscape ? 66 : 52;
    char v[16], lvl[48], name[48];
    for (int k = 0; k < n_tiles; k++) {
        int x = landscape ? 10 + k * (tw + gap) : 10;
        int y = landscape ? 38 : 38 + k * (th + gap);
        if (k == 0) {
            snprintf(v, sizeof(v), "%.0f" DEG, temp_unit(w->feels));
            snprintf(lvl, sizeof(lvl), "%s %.0f" DEG, T->wx_real, temp_unit(w->temp));
            wx_tile(s_wx_panel, x, y, tw, th, !landscape, T->wx_feels, v, 0xffffff, lvl, 0xbac4d6, -1);
        } else if (k == 1) {
            snprintf(v, sizeof(v), "%d", uvi);
            if (night_uv) {
                snprintf(name, sizeof(name), "%s \xC2\xB7 %s", T->wx_uv, T->wx_uv_max_fmt);
            } else {
                strlcpy(name, T->wx_uv, sizeof(name));
            }
            wx_tile(s_wx_panel, x, y, tw, th, !landscape, name, v, UV_COLOR[uvl], T->uv_level[uvl], UV_COLOR[uvl],
                    uvi * 100 / 11);
        } else {
            snprintf(v, sizeof(v), "%d", w->aqi);
            wx_tile(s_wx_panel, x, y, tw, th, !landscape, T->wx_air, v, AQI_COLOR[aql], T->aqi_level[aql],
                    AQI_COLOR[aql], w->aqi);
        }
    }

    // rain chance per hour: bar chart across the width
    int cx = 10, cw = sw - 20;
    int cy = landscape ? 38 + th + 6 : 38 + n_tiles * (th + gap) + 4;
    int bottom = sh - 20;   // hours below
    int n = w->n_rain;
    if (!landscape) {
        lv_obj_t *rt = label(s_wx_panel, &font_pt_12, COLOR_MUTED);
        lv_label_set_text(rt, T->wx_rain);
        lv_obj_set_pos(rt, cx, cy);
        cy += 16;
    }
    if (n <= 0) {
        return;
    }
    int bgap = landscape ? 8 : 5;
    int bw = (cw - bgap * (n - 1)) / n;
    int top = cy + 15;   // room for the % above the tallest bar
    int best = 0;
    for (int i = 1; i < n; i++) {
        if (w->rain_pct[i] > w->rain_pct[best]) {
            best = i;
        }
    }
    for (int i = 0; i < n; i++) {
        int pct = w->rain_pct[i] < 0 ? 0 : w->rain_pct[i] > 100 ? 100 : w->rain_pct[i];
        int bh = LV_MAX(3, (bottom - top) * pct / 100);
        int bx = cx + i * (bw + bgap);
        lv_obj_t *b = lv_obj_create(s_wx_panel);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, bw, bh);
        lv_obj_set_pos(b, bx, bottom - bh);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(pct >= 50 ? 0x1e88e5 : 0x42a5f5), 0);
        lv_obj_set_style_bg_opa(b, pct >= 20 ? LV_OPA_COVER : LV_OPA_50, 0);

        // % above the bar and the hour below, centered on the bar
        lv_obj_t *pl = label(s_wx_panel, &font_pt_12, i == best && pct > 0 ? lv_color_white() : COLOR_MUTED);
        label_fmt(pl, landscape ? "%d%%" : "%d", pct);
        lv_obj_update_layout(pl);
        lv_obj_set_pos(pl, bx + (bw - lv_obj_get_width(pl)) / 2, bottom - bh - 15);

        int hr = w->rain_hour[i];
        lv_obj_t *hl = label(s_wx_panel, &font_pt_12, COLOR_MUTED);
        if (g_cfg.h12) {
            label_fmt(hl, "%d%c", hr % 12 ? hr % 12 : 12, hr < 12 ? 'a' : 'p');
        } else {
            label_fmt(hl, landscape ? "%dh" : "%d", hr);
        }
        lv_obj_update_layout(hl);
        lv_obj_set_pos(hl, bx + (bw - lv_obj_get_width(hl)) / 2, bottom + 2);
    }
}

static void show_wx(void)
{
    int prev = overlay_now();
    s_idx = -1;
    s_card = false;
    s_card_night = false;
    s_card_sched = false;
    s_sv = false;
    s_sens = -1;
    s_city = -1;
    s_cmp = -1;
    s_work_page = false;
    s_wx_page = true;
    if (!s_wx_panel) {
        return;
    }
    render_wx();
    present(prev, OV_WX);
}

static void build_wx_panel(int w, int h)
{
    s_wx_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_wx_panel);
    lv_obj_set_size(s_wx_panel, w, h);
    lv_obj_set_style_bg_color(s_wx_panel, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_wx_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_wx_panel, LV_OBJ_FLAG_SCROLLABLE);
    if (s_wx_page && wx_available()) {
        render_wx();
    } else {
        s_wx_page = false;
        lv_obj_add_flag(s_wx_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------- "work" page ----------

static bool work_available(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return s_work.ok && tm.tm_year >= (2024 - 1900) && work_time(&tm, 0);
}

/*
 * Weather in the work city: now (icon, temperature, low/high, sky) and the next 12 hours every 2 hours
 * (bar = chance of rain, temperature above, hour below). Landscape: side by side; portrait: stacked.
 */
static void render_work(void)
{
    if (!s_work_panel || !s_work_page || !s_work.ok) {
        return;
    }
    lv_obj_clean(s_work_panel);
    const work_wx_t *k = &s_work;
    int w = lv_display_get_horizontal_resolution(NULL);
    int h = lv_display_get_vertical_resolution(NULL);
    bool landscape = w > h;

    lv_obj_t *title = label(s_work_panel, &font_pt_14, COLOR_ACCENT);
    scroll_line(title, w - 20);
    label_fmt(title, LV_SYMBOL_GPS "  %s \xC2\xB7 %s", T->work_title, k->name);
    lv_obj_set_pos(title, 10, 8);
    lv_obj_t *bar = lv_obj_create(s_work_panel);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, w - 20, 2);
    lv_obj_set_pos(bar, 10, 30);
    lv_obj_set_style_bg_color(bar, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);

    lv_obj_t *icon = weather_icon_create(s_work_panel);
    weather_icon_set(icon, weather_kind(k->code), k->is_day);
    lv_obj_set_pos(icon, 8, 40);
    lv_obj_t *temp = label(s_work_panel, &font_pt_28, lv_color_white());
    label_fmt(temp, "%.0f" DEG "%c", temp_unit(k->temp), s_fahrenheit ? 'F' : 'C');
    lv_obj_set_pos(temp, 60, 44);
    lv_obj_t *range = label(s_work_panel, &font_pt_14, COLOR_MUTED);
    label_fmt(range, T->city_minmax_fmt, temp_unit(k->tmin), temp_unit(k->tmax));
    lv_obj_set_pos(range, 10, 90);
    lv_obj_t *desc = label(s_work_panel, &font_pt_14, lv_color_white());
    scroll_line(desc, landscape ? 120 : w - 20);
    lv_label_set_text(desc, wmo_lookup(k->code).text);
    lv_obj_set_pos(desc, 10, 110);

    // next 12 hours: chance of rain as bars, temperature above, hour below
    int cx = landscape ? 140 : 10, cw = landscape ? w - cx - 10 : w - 20;
    int cy = landscape ? 38 : 140;
    lv_obj_t *ct = label(s_work_panel, &font_pt_12, COLOR_MUTED);
    one_line(ct, cw);
    lv_label_set_text(ct, T->work_chart);
    lv_obj_set_pos(ct, cx, cy);
    int n = k->n;
    if (n <= 0) {
        return;
    }
    int gap = 5, bw = (cw - gap * (n - 1)) / n;
    int bottom = h - 20, top = cy + 16 + 36;   // title, temperature and % above the bars
    for (int i = 0; i < n; i++) {
        int pct = k->pct[i] < 0 ? 0 : k->pct[i] > 100 ? 100 : k->pct[i];
        int bh = LV_MAX(3, (bottom - top) * pct / 100);
        int bx = cx + i * (bw + gap);
        lv_obj_t *b = lv_obj_create(s_work_panel);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, bw, bh);
        lv_obj_set_pos(b, bx, bottom - bh);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(pct >= 50 ? 0x1e88e5 : 0x42a5f5), 0);
        lv_obj_set_style_bg_opa(b, pct >= 20 ? LV_OPA_COVER : LV_OPA_50, 0);

        lv_obj_t *pl = label(s_work_panel, &font_pt_12, pct >= 50 ? lv_color_white() : COLOR_MUTED);
        label_fmt(pl, "%d%%", pct);
        lv_obj_update_layout(pl);
        lv_obj_set_pos(pl, bx + (bw - lv_obj_get_width(pl)) / 2, bottom - bh - 15);
        lv_obj_t *tl = label(s_work_panel, &font_pt_14, lv_color_white());
        label_fmt(tl, "%.0f" DEG, temp_unit(k->t[i]));
        lv_obj_update_layout(tl);
        lv_obj_set_pos(tl, bx + (bw - lv_obj_get_width(tl)) / 2, cy + 16);

        int hr = k->hour[i];
        lv_obj_t *hl = label(s_work_panel, &font_pt_12, COLOR_MUTED);
        if (g_cfg.h12) {
            label_fmt(hl, "%d%c", hr % 12 ? hr % 12 : 12, hr < 12 ? 'a' : 'p');
        } else {
            label_fmt(hl, "%dh", hr);
        }
        lv_obj_update_layout(hl);
        lv_obj_set_pos(hl, bx + (bw - lv_obj_get_width(hl)) / 2, bottom + 2);
    }
}

static void show_work(void)
{
    int prev = overlay_now();
    s_idx = -1;
    s_card = false;
    s_sens = -1;
    s_city = -1;
    s_wx_page = false;
    s_cmp = -1;
    s_work_page = true;
    if (!s_work_panel) {
        return;
    }
    render_work();
    present(prev, OV_WORK);
}

static void build_work_panel(int w, int h)
{
    s_work_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_work_panel);
    lv_obj_set_size(s_work_panel, w, h);
    lv_obj_set_style_bg_color(s_work_panel, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_work_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_work_panel, LV_OBJ_FLAG_SCROLLABLE);
    if (s_work_page && work_available()) {
        render_work();
    } else {
        s_work_page = false;
        lv_obj_add_flag(s_work_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_set_work(const work_wx_t *w)
{
    s_work = *w;
    render_work();
}

// ---------- sensor comparisons ----------

// The k-th comparison whose two sensors are on the screen now; false if there's none
static bool cmp_pair(int k, const govee_sensor_t **a, const govee_sensor_t **b)
{
    for (int i = 0; i < GOVEE_CMP_MAX; i++) {
        const govee_sensor_t *ga = NULL, *gb = NULL;
        for (int j = 0; j < s_ng; j++) {
            if (s_cmps[i].a[0] && strcmp(s_govee[j].device, s_cmps[i].a) == 0) {
                ga = &s_govee[j];
            }
            if (s_cmps[i].b[0] && strcmp(s_govee[j].device, s_cmps[i].b) == 0) {
                gb = &s_govee[j];
            }
        }
        if (ga && gb && k-- == 0) {
            *a = ga;
            *b = gb;
            return true;
        }
    }
    return false;
}

static int cmp_pages(void)
{
    const govee_sensor_t *a, *b;
    int n = 0;
    while (n < GOVEE_CMP_MAX && cmp_pair(n, &a, &b)) {
        n++;
    }
    return n;
}

static void cmp_side(lv_obj_t *parent, int x, int y, int w, int h, const govee_sensor_t *g)
{
    lv_obj_t *c = card(parent, x, y, w, h);
    lv_obj_t *n = label(c, &font_pt_12, COLOR_MUTED);
    scroll_line(n, w - 16);
    lv_label_set_text(n, g->name);
    lv_obj_t *t = label(c, &font_pt_20, lv_color_white());
    label_fmt(t, "%.1f" DEG, temp_unit(g->temp_c));
    lv_obj_set_pos(t, 0, 16);
    if (g->humidity >= 0) {
        lv_obj_t *hu = label(c, &font_pt_14, COLOR_MUTED);
        label_fmt(hu, "%.0f%%", g->humidity);
        lv_obj_set_pos(hu, 0, 42);
    }
}

/*
 * Landscape: A on the left, B on the right, the difference (A - B) big in the middle.
 * Portrait: A on top, the difference, B below.
 */
static void render_cmp(void)
{
    const govee_sensor_t *a, *b;
    if (!s_cmp_panel || s_cmp < 0 || !cmp_pair(s_cmp, &a, &b)) {
        return;
    }
    lv_obj_clean(s_cmp_panel);
    int w = lv_display_get_horizontal_resolution(NULL);
    int h = lv_display_get_vertical_resolution(NULL);
    bool landscape = w > h;

    lv_obj_t *title = label(s_cmp_panel, &font_pt_14, COLOR_ACCENT);
    scroll_line(title, w - 20);
    int pages = cmp_pages();
    if (pages > 1) {
        label_fmt(title, LV_SYMBOL_SHUFFLE "  %s  %d/%d", T->cmp_title, s_cmp + 1, pages);
    } else {
        label_fmt(title, LV_SYMBOL_SHUFFLE "  %s", T->cmp_title);
    }
    lv_obj_set_pos(title, 10, 8);
    lv_obj_t *bar = lv_obj_create(s_cmp_panel);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, w - 20, 2);
    lv_obj_set_pos(bar, 10, 30);
    lv_obj_set_style_bg_color(bar, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);

    // difference in the panel's unit: warmer orange, cooler blue
    float d = (a->temp_c - b->temp_c) * (s_fahrenheit ? 9.0f / 5.0f : 1.0f);
    bool same = d > -0.15f && d < 0.15f;
    lv_obj_t *dl = label(s_cmp_panel, &font_pt_28, same ? lv_color_white() :
                                                   d > 0 ? lv_color_hex(0xff8a65) : COLOR_ACCENT);
    if (same) {
        label_fmt(dl, "= 0" DEG);
    } else {
        label_fmt(dl, "%+.1f" DEG, d);
    }
    lv_obj_t *dh = label(s_cmp_panel, &font_pt_12, COLOR_MUTED);
    lv_obj_set_style_text_align(dh, LV_TEXT_ALIGN_CENTER, 0);
    if (same) {
        lv_label_set_text(dh, T->cmp_same);
    } else if (a->humidity >= 0 && b->humidity >= 0) {
        label_fmt(dh, T->cmp_hum_fmt, a->humidity - b->humidity);
    }
    if (landscape) {
        int cw = 96, ch = h - 46;
        cmp_side(s_cmp_panel, 10, 40, cw, ch, a);
        cmp_side(s_cmp_panel, w - 10 - cw, 40, cw, ch, b);
        lv_obj_align(dl, LV_ALIGN_CENTER, 0, 2);
        lv_obj_align(dh, LV_ALIGN_CENTER, 0, 32);
    } else {
        int cw = w - 20, ch = 80;
        cmp_side(s_cmp_panel, 10, 40, cw, ch, a);
        cmp_side(s_cmp_panel, 10, h - 8 - ch, cw, ch, b);
        lv_obj_align(dl, LV_ALIGN_CENTER, 0, -8);
        lv_obj_align(dh, LV_ALIGN_CENTER, 0, 22);
    }
}

static void show_cmp(int k)
{
    int prev = overlay_now();
    s_idx = -1;
    s_card = false;
    s_sens = -1;
    s_city = -1;
    s_wx_page = false;
    s_cmp = k;
    if (!s_cmp_panel) {
        return;
    }
    render_cmp();
    present(prev, OV_CMP);
}

static void build_cmp_panel(int w, int h)
{
    s_cmp_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_cmp_panel);
    lv_obj_set_size(s_cmp_panel, w, h);
    lv_obj_set_style_bg_color(s_cmp_panel, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_cmp_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_cmp_panel, LV_OBJ_FLAG_SCROLLABLE);
    if (s_cmp >= cmp_pages()) {
        s_cmp = -1;
    s_work_page = false;
    }
    if (s_cmp >= 0) {
        render_cmp();
    } else {
        lv_obj_add_flag(s_cmp_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------- other cities ----------

// Big clock: "14:05", or "2:05" with "PM" in a small label next to it (12-hour setting)
static void big_clock(lv_obj_t *time_label, lv_obj_t *ampm, int hour, int min)
{
    if (g_cfg.h12) {
        label_fmt(time_label, "%d:%02d", hour % 12 ? hour % 12 : 12, min);
        lv_label_set_text(ampm, hour < 12 ? "AM" : "PM");
        lv_obj_update_layout(time_label);
        lv_obj_align_to(ampm, time_label, LV_ALIGN_OUT_RIGHT_TOP, 4, 8);
    } else {
        label_fmt(time_label, "%02d:%02d", hour, min);
        lv_label_set_text(ampm, "");
    }
}

static void city_time_text(void)
{
    if (!s_city_time || s_city < 0 || s_city >= s_ncities) {
        return;
    }
    time_t t = time(NULL) + s_cities[s_city].utc_offset;
    struct tm tm;
    gmtime_r(&t, &tm);
    big_clock(s_city_time, s_city_ampm, tm.tm_hour, tm.tm_min);
}

/*
 * Portrait: name, big time, date + difference, weather card below.
 * Landscape: time and date on the left, weather card on the right.
 */
static void render_city(void)
{
    if (!s_city_panel || s_city < 0 || s_city >= s_ncities) {
        return;
    }
    lv_obj_clean(s_city_panel);
    const city_wx_t *c = &s_cities[s_city];
    int w = lv_display_get_horizontal_resolution(NULL);
    int h = lv_display_get_vertical_resolution(NULL);
    bool landscape = w > h;

    lv_obj_t *title = label(s_city_panel, &font_pt_14, COLOR_ACCENT);
    scroll_line(title, w - 20);
    label_fmt(title, LV_SYMBOL_GPS "  %s", c->name);
    lv_obj_set_pos(title, 10, 8);
    lv_obj_t *bar = lv_obj_create(s_city_panel);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, w - 20, 2);
    lv_obj_set_pos(bar, 10, 30);
    lv_obj_set_style_bg_color(bar, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);

    s_city_time = label(s_city_panel, &lv_font_montserrat_48, lv_color_white());
    s_city_ampm = label(s_city_panel, &font_pt_20, COLOR_ACCENT);

    // date there + difference to here ("+4h", "-5h30", "same time")
    time_t t = time(NULL) + c->utc_offset;
    struct tm tm;
    gmtime_r(&t, &tm);
    char date[48], diff[24];
    if (T->month_first) {
        snprintf(date, sizeof(date), "%s, %s %d", T->days[tm.tm_wday], T->months[tm.tm_mon], tm.tm_mday);
    } else {
        snprintf(date, sizeof(date), "%s, %d %s", T->days[tm.tm_wday], tm.tm_mday, T->months[tm.tm_mon]);
    }
    int d = (c->utc_offset - s_weather.utc_offset) / 60;
    if (!s_has_weather || d == 0) {
        strlcpy(diff, s_has_weather ? T->city_same_time : "", sizeof(diff));
    } else if (d % 60) {
        snprintf(diff, sizeof(diff), "%c%dh%02d", d > 0 ? '+' : '-', abs(d) / 60, abs(d) % 60);
    } else {
        snprintf(diff, sizeof(diff), "%c%dh", d > 0 ? '+' : '-', abs(d) / 60);
    }
    lv_obj_t *dl = label(s_city_panel, &font_pt_14, COLOR_MUTED);
    label_fmt(dl, "%s  \xC2\xB7  %s", date, diff);

    int cx, cy, cw, ch;
    city_time_text();   // sets the text, so the position below uses the real width
    lv_obj_update_layout(s_city_time);
    if (landscape) {
        lv_obj_set_pos(s_city_time, 12, 46);
        lv_obj_set_pos(dl, 14, 104);
        cx = 176, cy = 40, cw = w - 182, ch = h - 46;
    } else {
        lv_obj_align(s_city_time, LV_ALIGN_TOP_MID, 0, 50);
        lv_obj_align(dl, LV_ALIGN_TOP_MID, 0, 110);
        if (g_cfg.h12) {
            lv_obj_set_x(s_city_time, lv_obj_get_x(s_city_time) - 14);   // room for AM/PM on the right
        }
        cx = 6, cy = 146, cw = w - 12, ch = h - 152;
    }
    lv_obj_t *card_obj = card(s_city_panel, cx, cy, cw, ch);
    lv_obj_t *icon = weather_icon_create(card_obj);
    weather_icon_set(icon, weather_kind(c->code), c->is_day);
    lv_obj_t *temp = label(card_obj, &font_pt_28, lv_color_white());
    label_fmt(temp, "%.0f" DEG "%c", temp_unit(c->temp), s_fahrenheit ? 'F' : 'C');
    lv_obj_t *range = label(card_obj, &font_pt_14, COLOR_MUTED);
    label_fmt(range, T->city_minmax_fmt, temp_unit(c->tmin), temp_unit(c->tmax));   // no decimals: narrow card
    lv_obj_t *desc = label(card_obj, &font_pt_14, lv_color_white());
    one_line(desc, cw - 16);
    lv_label_set_text(desc, wmo_lookup(c->code).text);
    if (landscape) {
        lv_obj_set_pos(icon, 0, 0);
        lv_obj_set_pos(temp, 52, 6);
        lv_obj_set_pos(range, 0, 52);
        lv_obj_set_pos(desc, 0, 74);
    } else {
        lv_obj_set_pos(icon, 0, 4);
        lv_obj_set_pos(temp, 56, 8);
        lv_obj_set_pos(desc, 0, 56);
        lv_obj_set_pos(range, 0, 80);
    }
}

static void show_city(int i)
{
    int prev = overlay_now();
    s_idx = -1;
    s_card = false;
    s_card_night = false;
    s_card_sched = false;
    s_sv = false;
    s_sens = -1;
    s_city = i;
    s_cmp = -1;
    s_work_page = false;
    s_wx_page = false;
    if (!s_city_panel) {
        return;
    }
    render_city();
    present(prev, OV_CITY);
}

static void build_city_panel(int w, int h)
{
    s_city_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_city_panel);
    lv_obj_set_size(s_city_panel, w, h);
    lv_obj_set_style_bg_color(s_city_panel, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_city_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_city_panel, LV_OBJ_FLAG_SCROLLABLE);
    s_city_time = NULL;
    s_city_ampm = NULL;
    if (s_city >= s_ncities) {
        s_city = -1;
    s_cmp = -1;
    s_work_page = false;
    }
    if (s_city >= 0) {
        render_city();
    } else {
        lv_obj_add_flag(s_city_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_set_cities(const city_wx_t *c, int n)
{
    s_ncities = 0;
    for (int i = 0; i < n && s_ncities < CITIES_MAX; i++) {
        if (c[i].ok) {
            s_cities[s_ncities++] = c[i];
        }
    }
    if (s_city >= s_ncities) {
        show(-1);
    } else {
        render_city();
    }
}

static void roll_wait_cb(lv_timer_t *t)
{
    bool done = !s_browse || !s_sv || !s_sv_roll || s_browse_left > 0 ||   // left already, or the button was used
                (((lv_label_t *)s_msg_label)->offset.x == 0 &&             // the roll finished a full turn
                 ((lv_label_t *)s_msg_label)->offset.y == 0);
    if (!done) {
        return;
    }
    lv_timer_delete(t);
    s_roll_wait = NULL;
    if (s_browse && s_sv && s_browse_left <= 0) {
        exit_browse();
    }
}

// Normal cycle: dashboard, weather details, other cities, sensor pages, then each active message.
// Position: -1 dashboard, then cities, sensor pages and messages in a row.
static int cycle_pos(void)
{
    int nw = wx_available() + work_available(), nc = s_ncities, np = sens_pages(), nm = cmp_pages();
    return s_wx_page ? 0 : s_work_page ? nw - 1 : s_city >= 0 ? nw + s_city : s_sens >= 0 ? nw + nc + s_sens :
           s_cmp >= 0 ? nw + nc + np + s_cmp : s_idx >= 0 ? nw + nc + np + nm + s_idx : -1;
}

static void show_pos(int p)
{
    int nw = wx_available() + work_available(), nc = s_ncities, np = sens_pages(), nm = cmp_pages();
    if (p < 0) {
        s_left = s_dash_s;
        show(-1);
    } else if (p < nw) {
        s_left = s_wx_s;   // the work page takes the weather details' time
        if (p == 0 && wx_available()) {
            show_wx();
        } else {
            show_work();
        }
    } else if (p < nw + nc) {
        s_left = s_city_s;
        show_city(p - nw);
    } else if (p < nw + nc + np) {
        s_left = s_sens_s;
        show_sensors(p - nw - nc);
    } else if (p < nw + nc + np + nm) {
        s_left = s_sens_s;   // comparisons take the sensor pages' time
        show_cmp(p - nw - nc - np);
    } else {
        s_left = s_msg_s;
        show(p - nw - nc - np - nm);
    }
}

static void message_cycle(void)
{
    if (!s_msg_panel || s_deleting) {
        return;
    }
    if (s_browse) {
        if (--s_browse_left <= 0) {
            if (s_card && s_sv && s_sv_roll) {
                if (!s_roll_wait) {   // a long schedule rolling: leave when the text is back at the top
                    s_roll_wait = lv_timer_create(roll_wait_cb, 100, NULL);
                }
            } else {
                exit_browse();
            }
        }
        return;
    }
    if (s_night_rem || s_night_msg_left > 0) {
        return;   // night: only the new message, or the clock and the reminder taking turns (night_tick)
    }
    int total = wx_available() + work_available() + s_ncities + sens_pages() + cmp_pages() + s_n;
    if (total == 0) {
        if (overlay_now() != OV_NONE) {
            show(-1);
        }
        return;
    }
    if (--s_left > 0) {
        return;
    }
    int p = cycle_pos() + 1;
    show_pos(p < total ? p : -1);
}

// New or deleted message, or a schedule that started/ended
static void poll_cb(lv_timer_t *t)
{
    uint32_t ver = govee_version();
    if (ver != s_govee_ver && !s_config_mode) {
        s_govee_ver = ver;
        s_ng = govee_get(s_govee, GOVEE_MAX, false);
        if (s_sens >= sens_pages() || s_cmp >= cmp_pages()) {
            show(-1);
        } else {
            render_sensors();
            render_cmp();
        }
    }
    uint32_t ev = msg_take_events();
    if (!ev || s_config_mode) {
        return;
    }
    refresh_items();
    if ((ev & MSG_EV_NEW) && s_n > 0 && s_night_effective && s_wake_left == 0 && !s_browse && !s_deleting && !s_lan) {
        // night: the new message alone for a while (no weather pages), then the clock again
        s_night_msg_left = MSG_NEW_S;
        if (s_night && s_night_panel) {
            s_night = false;
            lv_obj_add_flag(s_night_panel, LV_OBJ_FLAG_HIDDEN);
        }
        show(0);
        return;
    }
    if ((ev & MSG_EV_NEW) && s_n > 0) {
        if (!s_browse && !s_deleting) {
            s_left = MSG_NEW_S;
            show(0);
            return;
        }
    }
    if (s_deleting || s_card) {
        return;
    }
    if (s_idx >= s_n) {
        if (s_browse && s_n > 0) {
            browse_show(s_n - 1);
        } else {
            s_browse = false;
            s_left = s_dash_s;
            show(-1);
        }
    } else if (s_idx >= 0) {
        render_message();
    }
}

bool ui_browse_next(void)
{
    if (s_config_mode || !s_msg_panel || s_deleting) {
        return false;
    }
    if (s_night) {   // at night the first click only wakes the panel up
        night_wake(WAKE_CLICK_S);
        return true;
    }
    if (!(s_card && s_card_night)) {
        s_wake_left = LV_MAX(s_wake_left, WAKE_CLICK_S);   // keep it awake while browsing
    }
    refresh_items();
    // sequence: message 1 .. message n, setup card, back to the dashboard
    if (!s_browse) {
        s_browse = true;
        s_browse_left = BROWSE_S;
        if (s_n > 0) {
            browse_show(0);
        } else {
            show_card();
        }
        return true;
    }
    // cards after the messages: setup -> night mode -> schedules -> dashboard
    s_browse_left = BROWSE_S;
    if (s_card && s_sv) {            // browsing the schedules: next one, then out
        if (s_sv_idx + 1 < s_sv_n) {
            s_sv_idx++;
            render_message();
            return true;
        }
        exit_browse();
        return false;
    }
    if (s_card && s_card_sched) {
        exit_browse();
        return false;
    }
    if (s_card && s_card_night) {    // night-mode card -> schedule card
        s_wake_left = 0;             // night mode just turned on there: don't keep the panel awake
        s_card_night = false;
        s_card_sched = true;
        s_sv_n = sched_list(s_sv_list);
        render_message();
        return true;
    }
    if (s_card) {                    // setup card -> night-mode card
        s_card_night = true;
        render_message();
        return true;
    }
    s_browse_left = BROWSE_S;
    if (s_idx + 1 < s_n) {
        browse_show(s_idx + 1);
    } else {
        show_card();
    }
    return true;
}

ui_hold_t ui_hold_action(void)
{
    if (!s_browse || s_deleting || s_lan) {
        return UI_HOLD_NONE;
    }
    if (s_card) {
        if (s_sv) {
            return s_sv_n ? UI_HOLD_ZONES : UI_HOLD_NONE;
        }
        if (s_card_sched) {
            return s_sv_n ? UI_HOLD_SCHED_OPEN : UI_HOLD_NONE;
        }
        return s_card_night ? UI_HOLD_NIGHT : UI_HOLD_LAN_SETUP;
    }
    return s_idx >= 0 && s_idx < s_n ? UI_HOLD_DELETE : UI_HOLD_NONE;
}

void ui_hold_progress(int percent)
{
    if (!s_del_bar) {
        return;
    }
    // red on a message (delete), blue on the setup card (open)
    // amber on the night-mode card
    lv_obj_set_style_bg_color(s_del_bar, lv_color_hex(s_card_night ? 0xc8823c : s_card ? 0x4fc3f7 : 0xff4d4f),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_del_bar, lv_color_hex(s_card_night ? 0x33240f : s_card ? 0x16324a : 0x3a1d24),
                              LV_PART_MAIN);
    if (percent < 0) {
        lv_obj_add_flag(s_del_bar, LV_OBJ_FLAG_HIDDEN);
        if (s_browse) {
            lv_obj_remove_flag(s_msg_hint, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    s_browse_left = BROWSE_S;
    lv_obj_add_flag(s_msg_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_del_bar, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(s_del_bar, percent, LV_ANIM_OFF);
}

static void deleted_done_cb(lv_timer_t *t)
{
    s_deleting = false;
    lv_obj_add_flag(s_del_done, LV_OBJ_FLAG_HIDDEN);
    refresh_items();
    msg_take_events();   // already refreshed here
    if (s_n == 0 || s_idx >= s_n) {
        exit_browse();   // it was the last one: back to the dashboard
    } else {
        s_browse_left = BROWSE_S;
        browse_show(s_idx);   // the next one takes the same slot
    }
}

void ui_delete_selected(void)
{
    if (ui_hold_action() != UI_HOLD_DELETE) {
        return;
    }
    msg_delete(s_items[s_idx].key);
    s_deleting = true;
    lv_obj_add_flag(s_del_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_msg_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_del_done, LV_OBJ_FLAG_HIDDEN);
    lv_obj_fade_in(s_del_done, 200, 0);
    lv_timer_t *t = lv_timer_create(deleted_done_cb, DELETED_MS, NULL);
    lv_timer_set_repeat_count(t, 1);
}

// ---------- night mode ----------

static bool night_hours(const struct tm *tm)
{
    return in_window(tm->tm_hour * 60 + tm->tm_min, s_night_from, s_night_to);
}

// Landscape: "03:47" across the screen and the date below. Portrait: hours above minutes.
// Digits only in the 100 px font; AM/PM goes with the date.
static void night_text(const struct tm *tm)
{
    if (!s_night_time) {
        return;
    }
    int w = lv_display_get_horizontal_resolution(NULL);
    int h = lv_display_get_vertical_resolution(NULL);
    bool landscape = w > h;
    int hour = g_cfg.h12 ? (tm->tm_hour % 12 ? tm->tm_hour % 12 : 12) : tm->tm_hour;
    const char *hfmt = g_cfg.h12 ? "%d" : "%02d";
    char hh[4];
    snprintf(hh, sizeof(hh), hfmt, hour);
    if (landscape) {
        label_fmt(s_night_time, "%s:%02d", hh, tm->tm_min);
        lv_label_set_text(s_night_min, "");
    } else {
        lv_label_set_text(s_night_time, hh);
        label_fmt(s_night_min, "%02d", tm->tm_min);
    }
    char date[48];
    if (T->month_first) {
        snprintf(date, sizeof(date), "%s, %s %d", T->days[tm->tm_wday], T->months[tm->tm_mon], tm->tm_mday);
    } else {
        snprintf(date, sizeof(date), "%s, %d %s", T->days[tm->tm_wday], tm->tm_mday, T->months[tm->tm_mon]);
    }
    if (g_cfg.h12) {
        label_fmt(s_night_date, "%s  \xC2\xB7  %s", date, tm->tm_hour < 12 ? "AM" : "PM");
    } else {
        lv_label_set_text(s_night_date, date);
    }

    if (landscape) {
        lv_obj_align(s_night_time, LV_ALIGN_CENTER, 0, -22);
        lv_obj_align(s_night_date, LV_ALIGN_CENTER, 0, 46);
    } else {
        lv_obj_align(s_night_time, LV_ALIGN_CENTER, 0, -70);
        lv_obj_align(s_night_min, LV_ALIGN_CENTER, 0, 18);
        lv_obj_align(s_night_date, LV_ALIGN_CENTER, 0, 100);
    }
}

static void build_night_panel(int w, int h)
{
    s_night_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_night_panel);
    lv_obj_set_size(s_night_panel, w, h);
    lv_obj_set_style_bg_color(s_night_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_night_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_night_panel, LV_OBJ_FLAG_SCROLLABLE);
    // big enough to read half asleep, from across the room
    s_night_time = label(s_night_panel, &font_clock_100, COLOR_NIGHT);
    s_night_min = label(s_night_panel, &font_clock_100, COLOR_NIGHT);
    s_night_date = label(s_night_panel, w > h ? &font_pt_28 : &font_pt_20, lv_color_hex(0x9a6a38));
    lv_obj_set_style_text_align(s_night_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_night_date, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_night_date, w - 12);
    if (!s_night) {
        lv_obj_add_flag(s_night_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

// A scheduled message nobody has read yet (e.g. "take the medicine") keeps the panel awake and the
// LED blinking until it's opened, deleted or its time is over. Direct messages only wake it briefly.
static bool sched_unread(void)
{
    for (int i = 0; i < s_n; i++) {
        if (s_items[i].scheduled && s_items[i].unread) {
            return true;
        }
    }
    return false;
}

// Called every second: night clock on/off by the hours, unless something woke the panel up
static void night_tick(const struct tm *tm, bool clock_ok)
{
    if (s_wake_left > 0) {
        s_wake_left--;
    }
    bool scheduled = s_night_on && clock_ok && night_hours(tm);
    if (!scheduled) {
        s_skip_night = false;   // "off for tonight" ends with the night hours
    }
    if (s_force_night && time(NULL) - s_force_since > FORCE_MAX_S) {
        s_force_night = false;  // forgotten: give the panel back
    }
    bool night = (scheduled && !s_skip_night) || s_force_night;
    if (night != s_night_effective) {
        s_night_effective = night;
        if (s_card_night) {
            render_message();
        }
    }
    bool reminder = night && sched_unread();
    led_set_urgent(reminder);   // a reminder at night: red breathing instead of the white blink
    bool awake = s_wake_left > 0 || s_lan || s_browse || s_deleting;
    if (s_night_msg_left > 0 && (!night || awake)) {
        s_night_msg_left = 0;
    }
    bool showing_new = s_night_msg_left > 0;
    if (showing_new) {
        s_night_msg_left--;
    }
    // reminder at night: the night clock and the reminder take turns, nothing else (a click shows the rest)
    s_night_rem = reminder && !awake && !showing_new;
    if (s_night_rem) {
        if (--s_rem_left <= 0) {
            s_rem_clock = !s_rem_clock;
            s_rem_left = s_rem_clock ? REM_CLOCK_S : REM_MSG_S;
            if (!s_rem_clock) {
                for (int i = 0; i < s_n; i++) {
                    if (s_items[i].scheduled && s_items[i].unread) {
                        show(i);
                        break;
                    }
                }
            }
        }
    } else {
        s_rem_clock = true;   // the turns start with the clock
        s_rem_left = REM_CLOCK_S;
    }
    bool want = night && !awake && !showing_new && (!reminder || s_rem_clock);
    led_set_quiet(night && !reminder && !awake);   // the red breathing goes on under the clock too
    if (want != s_night && s_night_panel) {
        s_night = want;
        if (want) {
            lv_obj_remove_flag(s_night_panel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_fade_in(s_night_panel, 600, 0);
        } else {
            lv_obj_add_flag(s_night_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_night) {
        night_text(tm);
    }
}

// Something to see at night (new message, click): show the normal panel for a while
static void night_wake(int seconds)
{
    if (s_wake_left < seconds) {
        s_wake_left = seconds;
    }
    if (s_night && s_night_panel) {
        s_night = false;
        led_set_quiet(false);
        lv_obj_add_flag(s_night_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

// Hold on the night-mode card: on now (a nap) or off (for the rest of tonight's hours)
void ui_night_toggle(void)
{
    if (s_night_effective) {
        s_force_night = false;
        s_skip_night = true;
        s_night_effective = false;
    } else {
        s_force_night = true;
        s_force_since = time(NULL);
        s_skip_night = false;
        s_night_effective = true;
        s_wake_left = 0;
    }
    s_wake_left = 0;        // this was the point of the clicks: don't keep the panel awake
    render_message();       // the card shows the new state for a moment...
    s_browse_left = 1;      // ...then back to normal, and the night clock comes up right away if it's on
}

// ---------- schedule browser ----------

void ui_sched_open(void)
{
    s_sv_n = sched_list(s_sv_list);
    if (!s_sv_n || !s_card_sched) {
        return;
    }
    s_card_sched = false;
    s_sv = true;
    s_sv_idx = 0;
    s_browse_left = BROWSE_S;
    render_message();
}

static int zone_of(int held_ms)
{
    return held_ms < ZONE_TODAY ? 0 : held_ms < ZONE_ALL ? 1 : held_ms < ZONE_NONE2 ? 2 : 3;
}

void ui_zone_progress(int held_ms)
{
    if (!s_sv || !s_zone) {
        return;
    }
    s_browse_left = BROWSE_S;
    static const uint32_t COL[] = {0xbac4d6, 0xff9800, 0xff4d4f, 0xbac4d6};
    const char *txt[] = {T->sv_z_none, T->sv_z_today, T->sv_z_all, T->sv_z_none};
    int z = zone_of(held_ms);
    int zw = lv_obj_get_width(s_zone);
    int x = (LV_MIN(held_ms, ZONE_END) - ZONE_START) * zw / (ZONE_END - ZONE_START);
    lv_obj_set_x(s_zone_mark, LV_MAX(0, LV_MIN(x, zw - 4)));
    lv_obj_add_flag(s_msg_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_sv_info, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_zone, LV_OBJ_FLAG_HIDDEN);
    if (strcmp(lv_label_get_text(s_zone_lbl), txt[z]) != 0) {
        lv_label_set_text(s_zone_lbl, txt[z]);
    }
    lv_obj_set_style_text_color(s_zone_lbl, lv_color_hex(COL[z]), 0);
    lv_obj_align(s_zone_lbl, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_remove_flag(s_zone_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void sv_done_cb(lv_timer_t *t)
{
    s_deleting = false;
    lv_obj_add_flag(s_del_done, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_del_txt, T->msg_deleted);
    s_sv_n = sched_list(s_sv_list);
    if (s_sv_n == 0) {
        exit_browse();
    } else {
        s_browse_left = BROWSE_S;
        render_message();
    }
}

void ui_zone_release(int held_ms)
{
    if (!s_sv || !s_zone) {
        return;
    }
    lv_obj_add_flag(s_zone, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_zone_lbl, LV_OBJ_FLAG_HIDDEN);
    int z = zone_of(held_ms);
    if ((z != 1 && z != 2) || s_sv_idx >= s_sv_n) {
        render_message();   // nothing: back to the schedule as it was
        return;
    }
    uint32_t id = s_sv_list[s_sv_idx].id;
    bool ok = z == 1 ? sched_skip_today(id) : sched_delete_id(id);
    if (!ok) {
        render_message();
        return;
    }
    lv_label_set_text(s_del_txt, z == 1 ? T->sv_done_today : T->sv_done_all);
    s_deleting = true;
    lv_obj_add_flag(s_msg_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_del_done, LV_OBJ_FLAG_HIDDEN);
    lv_obj_fade_in(s_del_done, 200, 0);
    lv_timer_t *t = lv_timer_create(sv_done_cb, DELETED_MS, NULL);
    lv_timer_set_repeat_count(t, 1);
}

bool ui_night_active(void)
{
    return s_night;
}

static void sig_sweep_cb(lv_timer_t *t)
{
    if (s_config_mode || !s_sig || s_sig_sweep < 0) {
        return;
    }
    s_sig_sweep = (s_sig_sweep + 1) % 5;   // 0..4 bars lit
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_bg_color(s_sig_bar[i], i < s_sig_sweep ? COLOR_ACCENT : lv_color_hex(0x3a4356), 0);
    }
}

static void tick_cb(lv_timer_t *t)
{
    if (s_config_mode) {
        return;
    }
    message_cycle();
    if (s_card && !s_card_night) {
        update_card_info();
    }
    city_time_text();
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    night_tick(&tm, tm.tm_year >= (2024 - 1900));
    if (tm.tm_year >= (2024 - 1900)) {
        big_clock(s_time, s_ampm, tm.tm_hour, tm.tm_min);   // AM/PM above the seconds
        label_fmt(s_sec, "%02d", tm.tm_sec);
        // the seconds follow the real width of HH:MM (Montserrat digits aren't monospaced)
        lv_obj_update_layout(s_time);
        lv_obj_align_to(s_sec, s_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 3, -9);
        if (T->month_first) {
            label_fmt(s_date, "%s, %s %d %d", T->days[tm.tm_wday], T->months[tm.tm_mon],
                                  tm.tm_mday, tm.tm_year + 1900);
        } else {
            label_fmt(s_date, "%s, %d %s %d", T->days[tm.tm_wday], tm.tm_mday,
                                  T->months[tm.tm_mon], tm.tm_year + 1900);
        }
    }

    wifi_ap_record_t ap;
    bool bars = false;
    if (s_toast[0] && lv_tick_elaps(s_toast_tick) < TOAST_MS) {
        lv_label_set_text(s_status, s_toast);
    } else if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        label_fmt(s_status, LV_SYMBOL_WIFI " %s", (const char *)ap.ssid);
        // signal as 1-4 bars, like a phone, instead of dBm
        int level = ap.rssi >= -55 ? 4 : ap.rssi >= -65 ? 3 : ap.rssi >= -75 ? 2 : 1;
        for (int i = 0; i < 4; i++) {
            lv_obj_set_style_bg_color(s_sig_bar[i], i < level ? lv_color_white() : lv_color_hex(0x3a4356), 0);
        }
        bars = true;
    } else {
        lv_label_set_text(s_status, s_status_text);
    }
    // connected: the real level; connecting or reconnecting: sweeping bars (sig_sweep_cb)
    if (bars) {
        s_sig_sweep = -1;
    } else if (s_sig_sweep < 0) {
        s_sig_sweep = 0;
    }
    bool toast = s_toast[0] && lv_tick_elaps(s_toast_tick) < TOAST_MS;
    if (toast) {
        lv_obj_add_flag(s_sig, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_sig, LV_OBJ_FLAG_HIDDEN);
    }
}

/*
 * Portrait (172x320)           Landscape (320x172)
 * +----------------+           +-------------+-------------+
 * | status         |           | status      | forecast    |
 * | 12:34 56       |           | 12:34 56    |  Today ...  |
 * | date           |           | date        |  Wed  ...   |
 * | [now]          |           | [now]       |  ...        |
 * | [5-day fcst]   |           +-------------+-------------+
 * +----------------+
 */
/*
 * Setup-mode screen: QR code to join the panel's Wi-Fi + instructions.
 * Portrait: QR on top, text below. Landscape: QR on the left, text on the right.
 */
static void build_config(lv_obj_t *scr, int w, int h)
{
    bool landscape = w > h;
    const int qr_size = landscape ? 124 : 128;
    const int qr_x = landscape ? 12 : (w - qr_size) / 2;
    const int qr_y = landscape ? (h - qr_size) / 2 : 40;
    const int tx = landscape ? 150 : 10;
    int ty = landscape ? 10 : 184;

    lv_obj_t *title = label(scr, &font_pt_20, COLOR_ACCENT);
    lv_label_set_text(title, T->cfg_title);
    if (landscape) {
        lv_obj_set_pos(title, tx, ty);
        ty += 34;
    } else {
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    }

    // white border around the QR (quiet zone) so cameras read it well
    lv_obj_t *qr = lv_qrcode_create(scr);
    lv_qrcode_set_size(qr, qr_size);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    const char *qr_data = "WIFI:T:nopass;S:" PORTAL_AP_SSID ";;";
    lv_qrcode_update(qr, qr_data, strlen(qr_data));
    lv_obj_set_pos(qr, qr_x, qr_y);
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 5, 0);

    const struct {
        const char *text;
        const lv_font_t *font;
        lv_color_t color;
        int gap;
    } lines[] = {
        {T->cfg_step1, &font_pt_12, COLOR_MUTED, 15},
        {PORTAL_AP_SSID, &font_pt_20, lv_color_white(), 28},
        {T->cfg_step2, &font_pt_12, COLOR_MUTED, 15},
        {PORTAL_AP_IP, &font_pt_20, lv_color_white(), 30},
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        lv_obj_t *l = label(scr, lines[i].font, lines[i].color);
        lv_label_set_text(l, lines[i].text);
        lv_obj_set_pos(l, tx, ty);
        ty += lines[i].gap;
    }

    s_cfg_msg = label(scr, &font_pt_12, COLOR_ACCENT);
    lv_label_set_long_mode(s_cfg_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_cfg_msg, w - tx - 6);
    lv_label_set_text(s_cfg_msg, T->cfg_exit);
    lv_obj_set_pos(s_cfg_msg, tx, ty);
}

static void build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_clean(lv_layer_top());
    s_msg_panel = NULL;
    s_sens_panel = NULL;
    s_city_panel = NULL;
    s_city_time = NULL;
    s_city_ampm = NULL;
    s_wx_panel = NULL;
    s_cmp_panel = NULL;
    s_work_panel = NULL;
    s_night_panel = NULL;
    s_night_time = s_night_min = s_night_date = NULL;
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);

    int w = lv_display_get_horizontal_resolution(NULL);
    int h = lv_display_get_vertical_resolution(NULL);
    if (s_config_mode) {
        build_config(scr, w, h);
        return;
    }
    bool landscape = w > h;
    int col_w = landscape ? 168 : w;   // clock + current weather column

    s_status = label(scr, &font_pt_14, COLOR_MUTED);
    one_line(s_status, col_w - 34);
    lv_obj_set_pos(s_status, 6, 2);
    s_sig = lv_obj_create(scr);
    lv_obj_remove_style_all(s_sig);
    lv_obj_set_size(s_sig, 19, 13);
    lv_obj_set_pos(s_sig, col_w - 25, 4);
    for (int i = 0; i < 4; i++) {
        s_sig_bar[i] = lv_obj_create(s_sig);
        lv_obj_remove_style_all(s_sig_bar[i]);
        lv_obj_set_size(s_sig_bar[i], 3, 4 + i * 3);
        lv_obj_set_style_radius(s_sig_bar[i], 1, 0);
        lv_obj_set_style_bg_opa(s_sig_bar[i], LV_OPA_COVER, 0);
        lv_obj_align(s_sig_bar[i], LV_ALIGN_BOTTOM_LEFT, i * 5, 0);
    }
    lv_obj_add_flag(s_sig, LV_OBJ_FLAG_HIDDEN);

    s_time = label(scr, &lv_font_montserrat_48, lv_color_white());
    lv_label_set_text(s_time, "--:--");
    lv_obj_set_pos(s_time, 8, 20);
    s_ampm = label(scr, &font_pt_14, COLOR_ACCENT);
    s_sec = label(scr, &font_pt_20, COLOR_ACCENT);
    lv_obj_align_to(s_sec, s_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 3, -9);
    s_date = label(scr, &font_pt_14, COLOR_MUTED);
    lv_label_set_text(s_date, T->waiting_time);
    lv_obj_set_pos(s_date, 10, 72);

    // weather now
    int now_h = landscape ? h - 96 : 92;
    lv_obj_t *now = card(scr, 6, 92, col_w - 12, now_h);
    // city, temperature and an animated icon instead of the weather's name
    lv_obj_t *city = label(now, &font_pt_14, COLOR_MUTED);
    scroll_line(city, col_w - 28);   // long names roll instead of being cut
    label_fmt(city, LV_SYMBOL_GPS " %s", g_cfg.city);
    s_temp = label(now, &font_pt_28, lv_color_white());
    lv_label_set_text(s_temp, "--.-" DEG);
    lv_obj_set_pos(s_temp, 0, landscape ? 22 : 17);
    s_wicon = weather_icon_create(now);
    lv_obj_align(s_wicon, LV_ALIGN_TOP_RIGHT, 2, landscape ? 16 : 10);
    s_extra = NULL;
    if (!landscape) {
        s_extra = label(now, &font_pt_14, COLOR_MUTED);
        lv_obj_set_pos(s_extra, 0, 56);
    }

    // forecast
    if (landscape) {
        s_forecast = card(scr, col_w, 4, w - col_w - 6, h - 8);
    } else {
        s_forecast = card(scr, 6, 92 + now_h + 6, w - 12, h - (92 + now_h + 6) - 4);
    }
    lv_obj_update_layout(s_forecast);

    render_current();
    render_forecast();
    build_message_panel(w, h);
    build_sensor_panel(w, h);
    build_city_panel(w, h);
    build_wx_panel(w, h);
    build_cmp_panel(w, h);
    build_work_panel(w, h);
    build_night_panel(w, h);
    s_lan_panel = NULL;   // lv_layer_top was cleaned above
    s_lan_msg = NULL;
    if (s_lan) {
        build_lan_panel(w, h);
    }
    tick_cb(NULL);
}

void ui_init(bool config_mode)
{
    s_config_mode = config_mode;
    s_dash_s = config_get_u8_def("t_dash", CYCLE_DEFAULT_S);
    s_sens_s = config_get_u8_def("t_sens", CYCLE_DEFAULT_S);
    s_msg_s = config_get_u8_def("t_msg", CYCLE_DEFAULT_S);
    s_city_s = config_get_u8_def("t_city", CYCLE_DEFAULT_S);
    s_wx_s = config_get_u8_def("t_wx", CYCLE_DEFAULT_S);
    s_wx_on = config_get_u8_def("wx_on", 1);
    govee_cmp_load(s_cmps);
    s_night_on = config_get_u8("night_on");
    s_night_from = config_get_u8_def("night_from", NIGHT_DEFAULT_FROM) % 24 * 60 + config_get_u8("night_fm") % 60;
    s_night_to = config_get_u8_def("night_to", NIGHT_DEFAULT_TO) % 24 * 60 + config_get_u8("night_tm") % 60;
    s_left = s_dash_s;
    strlcpy(s_status_text, T->starting, sizeof(s_status_text));
    build();
    lv_timer_create(tick_cb, 1000, NULL);
    lv_timer_create(sig_sweep_cb, 250, NULL);
    if (!config_mode) {
        lv_timer_create(poll_cb, 200, NULL);
    }
}

void ui_rebuild(void)
{
    build();
}

void ui_set_status(const char *text)
{
    strlcpy(s_status_text, text, sizeof(s_status_text));
    if (!s_config_mode) {
        lv_label_set_text(s_status, s_status_text);
    }
}

void ui_toast(const char *text)
{
    if (s_config_mode) {
        ui_config_message(text);
        return;
    }
    strlcpy(s_toast, text, sizeof(s_toast));
    s_toast_tick = lv_tick_get();
    lv_label_set_text(s_status, s_toast);
}

void ui_config_message(const char *text)
{
    if (s_config_mode && s_cfg_msg) {
        lv_label_set_text(s_cfg_msg, text);
    } else if (s_lan && s_lan_msg) {
        lv_label_set_text(s_lan_msg, text);
    }
}

void ui_set_weather(const weather_t *w)
{
    s_weather = *w;
    s_has_weather = true;
    render_current();
    render_forecast();
    render_wx();
}

void ui_set_fahrenheit(bool on)
{
    s_fahrenheit = on;
    if (!s_forecast || s_config_mode) {
        return;   // dashboard screen doesn't exist yet
    }
    char msg[24];
    snprintf(msg, sizeof(msg), T->unit_fmt, on ? 'F' : 'C');
    ui_toast(msg);
    render_current();
    render_forecast();
    render_sensors();
    render_city();
    render_wx();
    render_cmp();
    render_work();
}

// ---------- setup page on the home network ----------

// QR code with the board's address + instructions; same layout idea as the setup-mode screen
static void build_lan_panel(int w, int h)
{
    bool landscape = w > h;
    const int qr_size = landscape ? 120 : 128;

    s_lan_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_lan_panel);
    lv_obj_set_size(s_lan_panel, w, h);
    lv_obj_set_style_bg_color(s_lan_panel, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_lan_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_lan_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = label(s_lan_panel, &font_pt_14, COLOR_ACCENT);
    label_fmt(title, LV_SYMBOL_SETTINGS "  %s", T->card_title);
    lv_obj_set_pos(title, 10, 8);

    lv_obj_t *qr = lv_qrcode_create(s_lan_panel);
    lv_qrcode_set_size(qr, qr_size);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, s_lan_url, strlen(s_lan_url));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 5, 0);

    int tx = landscape ? qr_size + 26 : 10;
    int ty = landscape ? 40 : 40 + qr_size + 18;
    lv_obj_set_pos(qr, landscape ? 12 : (w - qr_size) / 2, landscape ? 36 : 34);

    lv_obj_t *step = label(s_lan_panel, &font_pt_12, COLOR_MUTED);
    lv_label_set_long_mode(step, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(step, w - tx - 8);
    lv_label_set_text(step, T->lan_step);
    lv_obj_set_pos(step, tx, ty);
    lv_obj_update_layout(step);
    ty += lv_obj_get_height(step) + 6;

    lv_obj_t *url = label(s_lan_panel, &font_pt_14, lv_color_white());
    lv_label_set_long_mode(url, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(url, w - tx - 8);
    lv_label_set_text(url, s_lan_url);
    lv_obj_set_pos(url, tx, ty);
    lv_obj_update_layout(url);
    ty += lv_obj_get_height(url) + 10;

    s_lan_msg = label(s_lan_panel, &font_pt_12, COLOR_ACCENT);
    lv_label_set_long_mode(s_lan_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_lan_msg, w - tx - 8);
    lv_label_set_text(s_lan_msg, T->lan_close);
    lv_obj_set_pos(s_lan_msg, tx, ty);
}

void ui_lan_setup_show(const char *url)
{
    strlcpy(s_lan_url, url, sizeof(s_lan_url));
    s_lan = true;
    s_browse = false;
    show(-1);
    if (!s_lan_panel) {
        build_lan_panel(lv_display_get_horizontal_resolution(NULL), lv_display_get_vertical_resolution(NULL));
    }
}

void ui_lan_setup_close(void)
{
    s_lan = false;
    s_lan_msg = NULL;
    if (s_lan_panel) {
        lv_obj_delete(s_lan_panel);
        s_lan_panel = NULL;
    }
    s_left = s_dash_s;
}

bool ui_lan_setup_active(void)
{
    return s_lan;
}
