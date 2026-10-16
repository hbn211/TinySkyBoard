#include "app.h"
#include "board.h"
#include "config.h"
#include "i18n.h"
#include "schedule.h"

#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"

#define COLOR_BG       lv_color_hex(0x0b0f1a)
#define COLOR_CARD     lv_color_hex(0x161d2e)
#define COLOR_MUTED    lv_color_hex(0x8a94a8)
#define COLOR_ACCENT   lv_color_hex(0x4fc3f7)
#define COLOR_UNREAD   lv_color_hex(0xffb74d)

#define DEG "\xC2\xB0"

LV_FONT_DECLARE(font_pt_12);
LV_FONT_DECLARE(font_pt_14);
LV_FONT_DECLARE(font_pt_20);
LV_FONT_DECLARE(font_pt_28);

#define DASH_SHOW_S    10   // cycle: dashboard 10 s, then each active message 6 s
#define MSG_SHOW_S     6
#define MSG_NEW_S      20   // a new message stays longer the first time
#define BROWSE_S       10   // browse mode (button) without a click goes back to normal
#define DELETED_MS     1200 // how long the "deleted" notice shows

// message text area: 36 px title on top, 22 px footer at the bottom
#define MSG_TOP        36
#define MSG_BOTTOM     22

static lv_obj_t *s_status;
static lv_obj_t *s_time;
static lv_obj_t *s_sec;
static lv_obj_t *s_date;
static lv_obj_t *s_temp;
static lv_obj_t *s_desc;
static lv_obj_t *s_extra;
static lv_obj_t *s_dot;
static lv_obj_t *s_forecast;
static lv_obj_t *s_cfg_msg;
static lv_obj_t *s_msg_panel;
static lv_obj_t *s_msg_title;
static lv_obj_t *s_msg_label;
static lv_obj_t *s_msg_when;
static lv_obj_t *s_msg_hint;      // browse-mode footer
static lv_obj_t *s_del_bar;       // "hold to delete" progress
static lv_obj_t *s_del_done;      // "deleted" notice
static bool s_config_mode;

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
static int s_left = DASH_SHOW_S;   // seconds until the next switch
static bool s_browse;       // browsing with the button
static int s_browse_left;
static bool s_deleting;     // showing the "deleted" notice

typedef struct {
    const char *text;
    uint32_t color;
} wmo_info_t;

static wmo_info_t wmo_lookup(int code)
{
    // T->weather indexes: clear, partly cloudy, cloudy, fog, drizzle, rain, snow, showers, thunderstorm
    static const uint32_t COLORS[] = {0xffd54f, 0xffe082, 0x90a4ae, 0xb0bec5, 0x81d4fa,
                                      0x42a5f5, 0xffffff, 0x1e88e5, 0xab47bc};
    int i = code == 0 ? 0 : code <= 2 ? 1 : code == 3 ? 2 : code <= 48 ? 3 : code <= 57 ? 4 :
            code <= 67 ? 5 : code <= 77 ? 6 : code <= 82 ? 7 : 8;
    return (wmo_info_t){T->weather[i], COLORS[i]};
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
    for (int i = 0; i < s_weather.n_days; i++) {
        const forecast_day_t *d = &s_weather.days[i];
        lv_obj_t *row = lv_obj_create(s_forecast);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), row_h);
        lv_obj_set_pos(row, 0, i * row_h);

        lv_obj_t *day = label(row, &font_pt_14, i == 0 ? COLOR_ACCENT : lv_color_white());
        lv_label_set_text(day, i == 0 ? T->today : T->days[d->wday]);
        lv_obj_align(day, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *icon = dot(row, 10, lv_color_hex(wmo_lookup(d->code).color));
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 46, 0);

        lv_obj_t *tmin = label(row, &font_pt_14, COLOR_MUTED);
        lv_label_set_text_fmt(tmin, "%.0f" DEG, temp_unit(d->temp_min));
        lv_obj_align(tmin, LV_ALIGN_RIGHT_MID, 0, 0);

        lv_obj_t *tmax = label(row, &font_pt_14, lv_color_white());
        lv_label_set_text_fmt(tmax, "%.0f" DEG, temp_unit(d->temp_max));
        lv_obj_align(tmax, LV_ALIGN_RIGHT_MID, -38, 0);
    }
}

static void render_current(void)
{
    if (!s_has_weather) {
        return;
    }
    wmo_info_t info = wmo_lookup(s_weather.code);
    lv_label_set_text_fmt(s_temp, "%.1f" DEG "%c", temp_unit(s_weather.temp), s_fahrenheit ? 'F' : 'C');
    lv_label_set_text(s_desc, info.text);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(info.color), 0);
    if (s_extra) {
        lv_label_set_text_fmt(s_extra, T->humidity_wind_fmt, s_weather.humidity, s_weather.wind_kmh);
    }
}

// Largest font in which the whole text fits an aw x ah area; NULL if not even the smallest fits
static const lv_font_t *fit_font(const char *text, int aw, int ah)
{
    static const lv_font_t *fonts[] = {&font_pt_28, &font_pt_20, &font_pt_14};
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
    return fit_font(text, LCD_H_RES - 24, LCD_V_RES - MSG_TOP - MSG_BOTTOM) &&
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

static void render_message(void)
{
    if (!s_msg_panel) {
        return;
    }
    if (s_idx < 0 || s_idx >= s_n) {
        lv_obj_add_flag(s_msg_panel, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const msg_item_t *m = &s_items[s_idx];

    char count[32] = "";
    if (s_n > 1) {
        snprintf(count, sizeof(count), "  %d/%d", s_idx + 1, s_n);
    }
    // unread (not opened with the button yet): orange header with a "new" tag
    lv_label_set_text_fmt(s_msg_title, "%s  %s%s%s%s", m->scheduled ? LV_SYMBOL_BELL : LV_SYMBOL_ENVELOPE,
                          T->msg_title, count, m->unread ? "  \xC2\xB7  " : "", m->unread ? T->msg_unread_tag : "");
    lv_obj_set_style_text_color(s_msg_title, m->unread ? COLOR_UNREAD : COLOR_ACCENT, 0);

    int w = lv_display_get_horizontal_resolution(NULL);
    int h = lv_display_get_vertical_resolution(NULL);
    int aw = w - 24, ah = h - MSG_TOP - MSG_BOTTOM;
    const lv_font_t *font = fit_font(m->text, aw, ah);
    lv_obj_set_style_text_font(s_msg_label, font ? font : &font_pt_14, 0);
    lv_label_set_long_mode(s_msg_label, font ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_msg_label, aw, font ? LV_SIZE_CONTENT : ah);
    lv_label_set_text(s_msg_label, m->text);
    lv_obj_align(s_msg_label, LV_ALIGN_CENTER, 0, (MSG_TOP - MSG_BOTTOM) / 2);

    if (m->when > 0) {
        struct tm t, today;
        time_t now = time(NULL);
        localtime_r(&m->when, &t);
        localtime_r(&now, &today);
        if (t.tm_yday == today.tm_yday && t.tm_year == today.tm_year) {
            lv_label_set_text_fmt(s_msg_when, T->msg_today_fmt, t.tm_hour, t.tm_min);
        } else {
            lv_label_set_text_fmt(s_msg_when, "%02d/%02d %02d:%02d", t.tm_mday, t.tm_mon + 1, t.tm_hour, t.tm_min);
        }
    } else {
        lv_label_set_text(s_msg_when, "");
    }

    if (s_browse) {
        lv_obj_remove_flag(s_msg_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_msg_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

// Switches what's on screen: dashboard (idx -1) or message idx, with a fade
static void show(int idx)
{
    if (!s_msg_panel) {
        s_idx = idx;
        return;
    }
    bool was_visible = !lv_obj_has_flag(s_msg_panel, LV_OBJ_FLAG_HIDDEN) && s_idx >= 0;
    s_idx = idx;
    if (idx < 0) {
        if (was_visible) {
            lv_obj_fade_out(s_msg_panel, 300, 0);
        }
        lv_obj_add_flag(s_msg_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    render_message();
    lv_obj_remove_flag(s_msg_panel, LV_OBJ_FLAG_HIDDEN);
    if (was_visible) {
        lv_obj_set_style_opa(s_msg_panel, LV_OPA_COVER, 0);
    } else {
        lv_obj_fade_in(s_msg_panel, 300, 0);
    }
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

static void exit_browse(void)
{
    s_browse = false;
    s_left = DASH_SHOW_S;
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

    s_msg_title = label(s_msg_panel, &font_pt_14, COLOR_ACCENT);
    lv_obj_set_pos(s_msg_title, 10, 8);
    s_msg_when = label(s_msg_panel, &font_pt_12, COLOR_MUTED);
    lv_obj_align(s_msg_when, LV_ALIGN_TOP_RIGHT, -10, 10);

    lv_obj_t *bar = lv_obj_create(s_msg_panel);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, w - 20, 2);
    lv_obj_set_pos(bar, 10, 30);
    lv_obj_set_style_bg_color(bar, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);

    s_msg_label = label(s_msg_panel, &font_pt_20, lv_color_white());
    lv_obj_set_style_text_align(s_msg_label, LV_TEXT_ALIGN_CENTER, 0);

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
    lv_obj_set_size(s_del_done, w, h - MSG_TOP);
    lv_obj_set_pos(s_del_done, 0, MSG_TOP);
    lv_obj_set_style_bg_color(s_del_done, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_del_done, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_del_done, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_del_done, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *icon = label(s_del_done, &font_pt_28, lv_color_hex(0xff4d4f));
    lv_label_set_text(icon, LV_SYMBOL_TRASH);
    lv_obj_t *txt = label(s_del_done, &font_pt_20, lv_color_hex(0xff4d4f));
    lv_label_set_text(txt, T->msg_deleted);
    lv_obj_add_flag(s_del_done, LV_OBJ_FLAG_HIDDEN);

    if (s_idx >= s_n) {
        s_idx = -1;
    }
    if (s_idx >= 0) {
        render_message();
        lv_obj_remove_flag(s_msg_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

// Normal cycle: dashboard DASH_SHOW_S, then each active message MSG_SHOW_S
static void message_cycle(void)
{
    if (!s_msg_panel || s_deleting) {
        return;
    }
    if (s_browse) {
        if (--s_browse_left <= 0) {
            exit_browse();
        }
        return;
    }
    if (s_n == 0) {
        if (s_idx >= 0) {
            show(-1);
        }
        return;
    }
    if (--s_left > 0) {
        return;
    }
    if (s_idx + 1 < s_n) {
        s_left = MSG_SHOW_S;
        show(s_idx + 1);
    } else {
        s_left = DASH_SHOW_S;
        show(-1);
    }
}

// New or deleted message, or a schedule that started/ended
static void poll_cb(lv_timer_t *t)
{
    uint32_t ev = msg_take_events();
    if (!ev || s_config_mode) {
        return;
    }
    refresh_items();
    if ((ev & MSG_EV_NEW) && s_n > 0) {
        if (!s_browse && !s_deleting) {
            s_left = MSG_NEW_S;
            show(0);
            return;
        }
    }
    if (s_deleting) {
        return;
    }
    if (s_idx >= s_n) {
        if (s_browse && s_n > 0) {
            browse_show(s_n - 1);
        } else {
            s_browse = false;
            s_left = DASH_SHOW_S;
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
    refresh_items();
    if (!s_browse) {
        if (s_n == 0) {
            ui_toast(T->msg_none);
            return false;
        }
        s_browse = true;
        s_browse_left = BROWSE_S;
        browse_show(0);
        return true;
    }
    if (s_idx + 1 >= s_n) {
        exit_browse();
        return false;
    }
    s_browse_left = BROWSE_S;
    browse_show(s_idx + 1);
    return true;
}

bool ui_browsing(void)
{
    return s_browse && !s_deleting && s_idx >= 0 && s_idx < s_n;
}

void ui_delete_progress(int percent)
{
    if (!s_del_bar) {
        return;
    }
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
    if (!ui_browsing()) {
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

static void tick_cb(lv_timer_t *t)
{
    if (s_config_mode) {
        return;
    }
    message_cycle();
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    if (tm.tm_year >= (2024 - 1900)) {
        lv_label_set_text_fmt(s_time, "%02d:%02d", tm.tm_hour, tm.tm_min);
        lv_label_set_text_fmt(s_sec, "%02d", tm.tm_sec);
        // the seconds follow the real width of HH:MM (Montserrat digits aren't monospaced)
        lv_obj_update_layout(s_time);
        lv_obj_align_to(s_sec, s_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 3, -9);
        if (T->month_first) {
            lv_label_set_text_fmt(s_date, "%s, %s %d %d", T->days[tm.tm_wday], T->months[tm.tm_mon],
                                  tm.tm_mday, tm.tm_year + 1900);
        } else {
            lv_label_set_text_fmt(s_date, "%s, %d %s %d", T->days[tm.tm_wday], tm.tm_mday,
                                  T->months[tm.tm_mon], tm.tm_year + 1900);
        }
    }

    wifi_ap_record_t ap;
    if (s_toast[0] && lv_tick_elaps(s_toast_tick) < TOAST_MS) {
        lv_label_set_text(s_status, s_toast);
    } else if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        lv_label_set_text_fmt(s_status, LV_SYMBOL_WIFI " %s  %d dBm", (const char *)ap.ssid, ap.rssi);
    } else {
        lv_label_set_text(s_status, s_status_text);
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

    s_status = label(scr, &font_pt_12, COLOR_MUTED);
    lv_obj_set_width(s_status, col_w - 12);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_status, 6, 4);

    s_time = label(scr, &lv_font_montserrat_48, lv_color_white());
    lv_label_set_text(s_time, "--:--");
    lv_obj_set_pos(s_time, 8, 20);
    s_sec = label(scr, &font_pt_20, COLOR_ACCENT);
    lv_obj_align_to(s_sec, s_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 3, -9);
    s_date = label(scr, &font_pt_14, COLOR_MUTED);
    lv_label_set_text(s_date, T->waiting_time);
    lv_obj_set_pos(s_date, 10, 72);

    // weather now
    int now_h = landscape ? h - 96 : 92;
    lv_obj_t *now = card(scr, 6, 92, col_w - 12, now_h);
    lv_obj_t *city = label(now, &font_pt_12, COLOR_MUTED);
    lv_label_set_text_fmt(city, LV_SYMBOL_GPS " %s", g_cfg.city);
    s_dot = dot(now, 14, COLOR_MUTED);
    lv_obj_align(s_dot, LV_ALIGN_TOP_RIGHT, 0, 0);
    s_temp = label(now, &font_pt_28, lv_color_white());
    lv_label_set_text(s_temp, "--.-" DEG);
    lv_obj_set_pos(s_temp, 0, 14);
    s_desc = label(now, landscape ? &font_pt_12 : &font_pt_14, lv_color_white());
    lv_obj_set_pos(s_desc, 0, landscape ? 46 : 48);
    s_extra = NULL;
    if (!landscape) {
        s_extra = label(now, &font_pt_12, COLOR_MUTED);
        lv_obj_set_pos(s_extra, 0, 64);
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
    tick_cb(NULL);
}

void ui_init(bool config_mode)
{
    s_config_mode = config_mode;
    strlcpy(s_status_text, T->starting, sizeof(s_status_text));
    build();
    lv_timer_create(tick_cb, 1000, NULL);
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
    }
}

void ui_set_weather(const weather_t *w)
{
    s_weather = *w;
    s_has_weather = true;
    render_current();
    render_forecast();
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
}
