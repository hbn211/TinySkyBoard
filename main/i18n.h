#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LANG_PT = 0,
    LANG_EN,
    LANG_ES,
    LANG_COUNT,
} lang_t;

// Every text the user sees: screen, setup page and bot replies
typedef struct {
    const char *code;           // "pt", "en", "es"
    const char *name;           // the language's name in itself

    // screen
    const char *days[7];
    const char *months[12];
    bool month_first;           // "Oct 13" (en) vs "13 out" (pt/es)
    const char *weather[9];     // clear, partly cloudy, cloudy, fog, drizzle, rain, snow, showers, thunderstorm
    const char *today;
    const char *starting;
    const char *waiting_time;
    const char *forecast_wait;
    const char *humidity_wind_fmt;  // %d humidity, %.0f wind
    const char *unit_fmt;           // %c = C/F
    const char *msg_title;
    const char *msg_today_fmt;      // %s = time
    const char *msg_none;
    const char *msg_browse_hint;
    const char *msg_deleted;
    const char *msg_unread_tag;     // "new" in the header of unread messages
    const char *card_title;         // browse card after the last message: setup on the home network
    const char *card_hint;
    const char *night_card_title;   // browse card after the setup card
    const char *night_card_on;
    const char *night_card_off;
    const char *night_card_hint;
    // status on the setup card
    const char *info_wifi_off;
    const char *info_weather_fmt;   // %s = time of the last update
    const char *info_weather_wait;
    const char *info_tg_off;
    const char *info_tg_wait;
    const char *info_tg_ok;
    const char *info_tg_fail;
    const char *info_gv_off;
    const char *info_gv_wait;
    const char *info_gv_key;        // the API refuses the key
    const char *info_gv_err_fmt;    // %d = HTTP status (-1 = no answer)
    const char *info_gv_ok_fmt;     // %d on screen, %d sensors
    const char *info_up_fmt;        // %s = uptime
    const char *lan_step;
    const char *lan_close;
    const char *lan_failed;
    const char *cfg_title;
    const char *cfg_step1;
    const char *cfg_step2;
    const char *cfg_exit;
    const char *hold_config;
    const char *hold_exit;
    const char *restarting;
    const char *saved_restarting;
    const char *wifi_lost;
    const char *no_wifi_portal;
    const char *finding_city;
    const char *city_not_found_fmt; // %s = city
    const char *wx_title;           // weather details page
    const char *wx_feels;
    const char *wx_uv;
    const char *wx_air;
    const char *wx_rain;            // "Rain, next 6 h"
    const char *wx_uv_max_fmt;      // "today's max" (UV shown at night)
    const char *uv_level[5];        // low, moderate, high, very high, extreme
    const char *aqi_level[6];       // good, fair, moderate, poor, very poor, extremely poor
    const char *city_same_time;
    const char *city_minmax_fmt;    // %.0f min, %.0f max     // other city in the same time zone
    const char *sens_title;         // Govee sensors screen
    const char *sens_updated_fmt;   // %s = time of the last new reading
    const char *sens_wait;
    const char *sens_minmax_fmt;    // %.1f min, %.1f max (no unit)
    const char *sens_hum_fmt;       // %.0f humidity

    // setup page
    const char *p_title;
    const char *p_network;
    const char *p_network_hint;
    const char *p_password;
    const char *p_password_hint;
    const char *p_city;
    const char *p_city_hint;
    const char *p_unit;
    const char *p_clock;
    const char *p_language;
    const char *p_tg;
    const char *p_tg_set;
    const char *p_tg_hint;
    const char *p_night_on;
    const char *p_night_hours;
    const char *p_night_hint;
    const char *p_wx_on;
    const char *p_t_wx;
    const char *p_cities;
    const char *p_cities_hint;
    const char *p_t_city;
    const char *p_govee;
    const char *p_govee_hint;
    const char *p_govee_int;
    const char *p_govee_int_hint;
    const char *p_save;
    const char *p_required;
    const char *p_saved_title;
    const char *p_saved_pre;
    const char *p_saved_post;
    const char *p_bright_day;
    const char *p_bright_night;
    const char *p_bright_led;
    const char *p_bright_led_night;
    const char *p_bright_alert;
    const char *p_bright_hint;
    const char *p_t_dash;
    const char *p_t_sens;
    const char *p_t_msg;
    const char *p_t_hint;
    const char *p_rotation;
    const char *p_rotation_hint;
    const char *p_usb[4];         // USB at the bottom, left, top, right
    const char *p_btn_legend;
    const char *p_help_link;
    const char *p_back_link;

    // Telegram bot
    const char *tg_welcome;
    const char *tg_help;
    const char *tg_private;
    const char *tg_only_text;
    const char *tg_cleared;
    const char *tg_no_glyphs;
    const char *tg_msgs_title;      // /mensagens
    const char *tg_msg_read;
    const char *tg_msg_unread;
    const char *tg_some_dropped;    // part of the message (emoji the panel doesn't have) was left out
    const char *tg_ok;
    const char *tg_ok_count_fmt;    // %d active messages
    const char *tg_too_long_fmt;    // %d characters, %d fit

    // scheduling (/agendar)
    const char *tomorrow;           // "tomorrow" for showing dates
    const char *sch_ask_text;
    const char *sch_ask_when;
    const char *sch_ask_dur;
    const char *sch_ask_repeat;
    const char *sch_bad_when;
    const char *sch_bad_dur;
    const char *sch_past;
    const char *sch_full;
    const char *sch_done;
    const char *sch_aborted;
    const char *sch_empty;
    const char *sch_list_title;
    const char *sch_on_now;
    const char *sch_removed_fmt;    // %d
    const char *sch_bad_index;
    const char *sch_no_clock;
    const char *btn_now;
    const char *btn_in30;           // must start with the language's word for "in"
    const char *btn_day;            // "1 day"
    const char *repeat[4];          // once, every day, Mon-Fri, weekends

    // power-on/back-online notices and /status
    const char *st_boot;            // "Panel on"
    const char *st_reason[6];       // power, panel restart, error, brownout, USB, other
    const char *st_was_off_fmt;     // %s = since (date/time), %s = duration
    const char *st_back_fmt;        // %s = duration, %s = since (time)
    const char *st_online;          // "Online"
    const char *st_uptime;
    const char *st_signal;
    const char *st_weather;
    const char *st_no_weather;
    const char *st_memory;
    const char *st_sens_gone;       // sensor without new readings, off the screen
} i18n_t;

extern const i18n_t *T;     // active language

void i18n_set(uint8_t lang);
const i18n_t *i18n_get(uint8_t lang);
