#include "app.h"
#include "config.h"
#include "i18n.h"
#include "schedule.h"
#include "emoji/emoji.h"

#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs.h"
#include "cJSON.h"

#define POLL_TIMEOUT_S  25      // long polling: Telegram holds the connection until something arrives
#define CLEAN_MAX       1024    // cleaned text before checking it fits the screen
#define WIZARD_IDLE_S   (10 * 60)   // an abandoned assistant goes back to normal
#define OUTAGE_MIN_S    60          // outages shorter than this aren't reported
#define ALIVE_SAVE_S    (5 * 60)    // "still alive" mark in NVS, to measure how long it was off

static const char *TAG = "telegram";

// ---------- sending ----------

// Sends text; with buttons, shows a quick-reply keyboard (per_row per row)
static void send_kb(int64_t chat_id, const char *text, const char *const *buttons, int n, int per_row)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "chat_id", (double)chat_id);
    cJSON_AddStringToObject(body, "text", text);
    cJSON *markup = cJSON_AddObjectToObject(body, "reply_markup");
    if (n > 0) {
        cJSON *rows = cJSON_AddArrayToObject(markup, "keyboard");
        cJSON *row = NULL;
        for (int i = 0; i < n; i++) {
            if (i % per_row == 0) {
                row = cJSON_CreateArray();
                cJSON_AddItemToArray(rows, row);
            }
            cJSON_AddItemToArray(row, cJSON_CreateString(buttons[i]));
        }
        cJSON_AddTrueToObject(markup, "one_time_keyboard");
        cJSON_AddTrueToObject(markup, "resize_keyboard");
    } else {
        cJSON_AddTrueToObject(markup, "remove_keyboard");
    }
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    char url[160];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", g_cfg.tg_token);
    cJSON_Delete(net_http_json(url, json, 10000));
    free(json);
}

static void reply(int64_t chat_id, const char *text)
{
    send_kb(chat_id, text, NULL, 0, 1);
}

// Telegram "/" menu with the commands in the panel's language
static void set_commands(void)
{
    static const char *const CMDS[LANG_COUNT][8][2] = {
        [LANG_PT] = {{"mensagens", "Mensagens na tela e se foram lidas"},
                     {"chuva", "Ligar/desligar o aviso de chuva"},
                     {"agendar", "Programar uma mensagem"}, {"agenda", "Ver o que está agendado"},
                     {"cancelar", "Apagar um agendamento: /cancelar N"}, {"limpar", "Apagar as mensagens enviadas (não as agendadas)"},
                     {"status", "Ver se o painel está ligado"}, {"ajuda", "Ajuda"}},
        [LANG_EN] = {{"messages", "Messages on the panel and whether they were read"},
                     {"rain", "Turn the rain alert on/off"},
                     {"schedule", "Schedule a message"}, {"list", "See what is scheduled"},
                     {"cancel", "Delete a schedule: /cancel N"}, {"clear", "Delete the sent messages (not the scheduled ones)"},
                     {"status", "Check that the panel is on"}, {"help", "Help"}},
        [LANG_ES] = {{"mensajes", "Mensajes en el panel y si se leyeron"},
                     {"lluvia", "Activar/desactivar el aviso de lluvia"},
                     {"programar", "Programar un mensaje"}, {"lista", "Ver lo programado"},
                     {"cancelar", "Borrar una programación: /cancelar N"}, {"borrar", "Borrar los mensajes enviados (no los programados)"},
                     {"estado", "Ver si el panel está encendido"}, {"ayuda", "Ayuda"}},
    };
    int lang = 0;
    for (int i = 0; i < LANG_COUNT; i++) {
        if (i18n_get(i) == T) {
            lang = i;
        }
    }
    cJSON *body = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(body, "commands");
    for (int i = 0; i < 8; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "command", CMDS[lang][i][0]);
        cJSON_AddStringToObject(c, "description", CMDS[lang][i][1]);
        cJSON_AddItemToArray(list, c);
    }
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    char url[160];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/setMyCommands", g_cfg.tg_token);
    cJSON_Delete(net_http_json(url, json, 10000));
    free(json);
}

// ---------- text ----------

// Keeps only what the panel fonts can draw (ASCII + Latin-1 + quotes/dashes/ellipsis + the color
// emoji in main/emoji). Returns how many characters were dropped. Variation selectors and skin tones
// are left out without counting (the emoji shows in its default look); in combined emoji
// (joined with ZWJ) only the first one stays.
static int sanitize(const char *in, char *out, size_t size)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t o = 0;
    int dropped = 0;
    bool joined = false;   // the previous character was a ZWJ: skip this one too
    while (*p && o + 4 < size) {
        uint32_t cp;
        int n;
        if (*p < 0x80) {
            cp = *p;
            n = 1;
        } else if ((*p & 0xe0) == 0xc0) {
            cp = *p & 0x1f;
            n = 2;
        } else if ((*p & 0xf0) == 0xe0) {
            cp = *p & 0x0f;
            n = 3;
        } else {
            cp = *p & 0x07;
            n = 4;
        }
        for (int i = 1; i < n; i++) {
            if ((p[i] & 0xc0) != 0x80) {
                n = i;   // broken UTF-8: skip the garbage
                cp = 0;
                break;
            }
            cp = (cp << 6) | (p[i] & 0x3f);
        }
        bool silent = cp == 0xFE0F || cp == 0x200D || (cp >= 0x1F3FB && cp <= 0x1F3FF);
        bool keep = cp == '\n' || (cp >= 0x20 && cp <= 0x7e) || (cp >= 0xa0 && cp <= 0xff) ||
                    cp == 0x2013 || cp == 0x2014 || cp == 0x2018 || cp == 0x2019 ||
                    cp == 0x201c || cp == 0x201d || cp == 0x2022 || cp == 0x2026 || cp == 0x20ac ||
                    emoji_known(cp);
        if (joined) {
            keep = false;
            silent = true;
        }
        joined = cp == 0x200D;
        if (keep) {
            memcpy(out + o, p, n);
            o += n;
        } else if (!silent && cp) {
            dropped++;
        }
        p += n;
    }
    out[o] = '\0';

    // trim leftover spaces at the ends (e.g. an emoji at the end)
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\n')) {
        out[--o] = '\0';
    }
    size_t start = strspn(out, " \n");
    memmove(out, out + start, o - start + 1);
    return dropped;
}

// lowercase ASCII only (accents stay as they are)
static void lower(char *s)
{
    for (; *s; s++) {
        *s = tolower((unsigned char)*s);
    }
}

// "/agendar@MyBot 3" -> cmd "/agendar", arg "3"
static bool is_cmd(const char *cmd, const char *const *names, int n)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(cmd, names[i]) == 0) {
            return true;
        }
    }
    return false;
}

// ---------- duration and time parsing ----------

// "30 min", "2h", "1h30", "1 dia", "90" -> minutes; 0 if not understood
static int parse_duration(const char *in)
{
    char s[48];
    strlcpy(s, in, sizeof(s));
    lower(s);
    const char *p = s;
    int total = 0;
    bool any = false;
    while (*p) {
        while (*p && !isdigit((unsigned char)*p)) {
            p++;
        }
        if (!*p) {
            break;
        }
        int n = strtol(p, (char **)&p, 10);
        while (*p == ' ') {
            p++;
        }
        char unit = *p;   // m(in), h(our/ora), d(ay/ia/ía)
        if (unit == 'm' || unit == 'h' || unit == 'd') {
            while (*p && isalpha((unsigned char)*p)) {
                p++;
            }
        } else {
            unit = 'm';   // "1h30" or a bare number: minutes
        }
        total += unit == 'd' ? n * 1440 : unit == 'h' ? n * 60 : n;
        any = true;
    }
    return any ? total : 0;
}

static bool word_in(const char *s, const char *const *words, int n, const char **after)
{
    for (int i = 0; i < n; i++) {
        size_t len = strlen(words[i]);
        if (strncmp(s, words[i], len) == 0 && (s[len] == '\0' || s[len] == ' ')) {
            if (after) {
                *after = s + len;
            }
            return true;
        }
    }
    return false;
}

// "18:30", "18h", "amanhã 08:00", "25/12 09:00", "em 30 min", "agora" (and en/es equivalents)
// Fills in the start and local time; false if not understood.
static bool parse_when(const char *in, time_t now, time_t *start, int *hour, int *minute)
{
    static const char *const NOW[] = {"agora", "now", "ahora"};
    static const char *const IN[] = {"em", "in", "en", "daqui"};
    static const char *const TOMORROW[] = {"amanhã", "amanha", "tomorrow", "mañana", "manana"};

    char s[48];
    strlcpy(s, in, sizeof(s));
    lower(s);
    char *p = s + strspn(s, " ");
    const char *rest;
    struct tm t;

    if (word_in(p, NOW, 3, NULL)) {
        *start = now;
    } else if (word_in(p, IN, 4, &rest)) {
        int min = parse_duration(rest);
        if (min <= 0) {
            return false;
        }
        *start = now + min * 60;
    } else {
        bool tomorrow = false, has_date = false, has_time = false, has_year = false;
        int day = 0, mon = 0, year = 0, h = 0, m = 0;
        for (char *tok = strtok(p, " "); tok; tok = strtok(NULL, " ")) {
            if (word_in(tok, TOMORROW, 5, NULL)) {
                tomorrow = true;
            } else if (strchr(tok, '/')) {
                int a = 0, b = 0, c = 0;
                int n = sscanf(tok, "%d/%d/%d", &a, &b, &c);
                if (n < 2) {
                    return false;
                }
                day = T->month_first ? b : a;
                mon = T->month_first ? a : b;
                if (n == 3) {
                    year = c < 100 ? c + 2000 : c;
                    has_year = true;
                }
                has_date = true;
            } else if (isdigit((unsigned char)tok[0])) {
                char sep = 0;
                int n = sscanf(tok, "%d%c%d", &h, &sep, &m);
                if (n == 1 || (n == 2 && sep == 'h')) {
                    m = 0;
                } else if (n != 3 || !strchr(":h.", sep)) {
                    return false;
                }
                has_time = true;
            }
        }
        if (!has_time || h > 23 || m > 59 || (has_date && (day < 1 || day > 31 || mon < 1 || mon > 12))) {
            return false;
        }
        localtime_r(&now, &t);
        if (has_date) {
            t.tm_mday = day;
            t.tm_mon = mon - 1;
            if (has_year) {
                t.tm_year = year - 1900;
            }
        } else if (tomorrow) {
            t.tm_mday += 1;
        }
        t.tm_hour = h;
        t.tm_min = m;
        t.tm_sec = 0;
        t.tm_isdst = -1;
        *start = mktime(&t);
        if (*start <= now && !has_date && !tomorrow) {
            t.tm_mday += 1;   // "18:30" already past = tomorrow
            t.tm_isdst = -1;
            *start = mktime(&t);
        } else if (*start <= now && has_date && !has_year) {
            t.tm_year += 1;   // "25/12" already past = next year
            t.tm_isdst = -1;
            *start = mktime(&t);
        }
    }
    localtime_r(start, &t);
    *hour = t.tm_hour;
    *minute = t.tm_min;
    return true;
}

// ---------- formatting ----------

static void fmt_duration(int min, char *out, size_t size)
{
    if (min % 1440 == 0) {
        snprintf(out, size, "%d d", min / 1440);
    } else if (min % 60 == 0) {
        snprintf(out, size, "%d h", min / 60);
    } else if (min > 60) {
        snprintf(out, size, "%dh%02d", min / 60, min % 60);
    } else {
        snprintf(out, size, "%d min", min);
    }
}

// "Hoje 18:30 · 1 h · Uma vez"
static void fmt_entry(const sched_t *s, char *out, size_t size)
{
    char when[40], dur[16];
    if (s->repeat == REPEAT_ONCE) {
        time_t now = time(NULL), start = (time_t)s->start;
        struct tm t, today;
        localtime_r(&start, &t);
        localtime_r(&now, &today);
        time_t tomorrow_t = now + 86400;
        struct tm tom;
        localtime_r(&tomorrow_t, &tom);
        char hm[12];
        clock_fmt(hm, sizeof(hm), t.tm_hour, t.tm_min);
        if (t.tm_yday == today.tm_yday && t.tm_year == today.tm_year) {
            snprintf(when, sizeof(when), "%s %s", T->today, hm);
        } else if (t.tm_yday == tom.tm_yday && t.tm_year == tom.tm_year) {
            snprintf(when, sizeof(when), "%s %s", T->tomorrow, hm);
        } else if (T->month_first) {
            snprintf(when, sizeof(when), "%02d/%02d %s", t.tm_mon + 1, t.tm_mday, hm);
        } else {
            snprintf(when, sizeof(when), "%02d/%02d %s", t.tm_mday, t.tm_mon + 1, hm);
        }
    } else {
        clock_fmt(when, sizeof(when), s->hour, s->minute);
    }
    fmt_duration(s->dur_min, dur, sizeof(dur));
    snprintf(out, size, "%s \xC2\xB7 %s \xC2\xB7 %s", when, dur, T->repeat[s->repeat]);
}

static void send_list(int64_t chat_id)
{
    sched_t list[SCHED_MAX];
    int n = sched_list(list);
    if (n == 0) {
        reply(chat_id, T->sch_empty);
        return;
    }
    char out[SCHED_MAX * (SCHED_TEXT + 80) + 32];
    size_t o = snprintf(out, sizeof(out), "%s\n", T->sch_list_title);
    for (int i = 0; i < n && o < sizeof(out); i++) {
        char line[80];
        fmt_entry(&list[i], line, sizeof(line));
        // same rule as the screen: an occurrence deleted with the button isn't "on the panel"
        int st = sched_state(list[i].id);
        const char *tag = st == SCHED_ON_SCREEN ? T->sch_on_now : st == SCHED_DISMISSED ? T->sch_dismissed :
                          st == SCHED_SKIPPED ? T->sch_skipped : NULL;
        o += snprintf(out + o, sizeof(out) - o, "\n%d. %s%s%s%s\n   \xE2\x80\x9C%s\xE2\x80\x9D\n", i + 1, line,
                      tag ? "  (" : "", tag ? tag : "", tag ? ")" : "", list[i].text);
    }
    reply(chat_id, out);
}

// ---------- power-on / back online / status ----------

static int64_t s_prev_alive;        // last "still alive" mark saved before this boot
static time_t s_last_alive_save;

static int64_t uptime_s(void)
{
    return esp_timer_get_time() / 1000000;
}

static bool clock_ok(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_year >= (2024 - 1900);
}

// 45 -> "45 s", 750 -> "12 min", 7800 -> "2 h 10 min", 200000 -> "2 d 7 h"
static void fmt_span(int64_t s, char *out, size_t size)
{
    if (s < 60) {
        snprintf(out, size, "%d s", (int)s);
    } else if (s < 3600) {
        snprintf(out, size, "%d min", (int)(s / 60));
    } else if (s < 86400) {
        snprintf(out, size, "%d h %d min", (int)(s / 3600), (int)(s % 3600 / 60));
    } else {
        snprintf(out, size, "%d d %d h", (int)(s / 86400), (int)(s % 86400 / 3600));
    }
}

// "14:02" if today, otherwise "13/10 14:02" (10/13 in English)
static void fmt_since(time_t t, char *out, size_t size)
{
    time_t now = time(NULL);
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    char hm[12];
    clock_fmt(hm, sizeof(hm), a.tm_hour, a.tm_min);
    if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year) {
        snprintf(out, size, "%s", hm);
    } else {
        snprintf(out, size, "%02d/%02d ", T->month_first ? a.tm_mon + 1 : a.tm_mday,
                 T->month_first ? a.tm_mday : a.tm_mon + 1);
        strlcat(out, hm, size);
    }
}

static void save_alive(void)
{
    time_t now = time(NULL);
    if (!clock_ok(now) || now - s_last_alive_save < ALIVE_SAVE_S) {
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open("painel", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i64(nvs, "alive", now);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    s_last_alive_save = now;
}

static void announce_boot(void)
{
    int r;
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: r = 0; break;
    case ESP_RST_SW: r = 1; break;
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: r = 2; break;
    case ESP_RST_BROWNOUT: r = 3; break;
    case ESP_RST_USB: r = 4; break;
    default: r = 5; break;
    }
    char out[200];
    int n = snprintf(out, sizeof(out), "\xE2\x9C\x85 %s (%s).", T->st_boot, T->st_reason[r]);

    // how long it was off: from the last "still alive" mark until now (~5 min precision)
    time_t now = time(NULL);
    if (s_prev_alive > 0 && clock_ok(now) && now - s_prev_alive > 2 * ALIVE_SAVE_S) {
        char since[24], span[24];
        fmt_since((time_t)s_prev_alive, since, sizeof(since));
        fmt_span(now - s_prev_alive - uptime_s(), span, sizeof(span));
        out[n++] = '\n';
        snprintf(out + n, sizeof(out) - n, T->st_was_off_fmt, since, span);
    }
    ESP_LOGI(TAG, "boot notice: %s", out);
    reply(g_cfg.tg_owner, out);
}

// snprintf at out[*n] that never runs past the buffer (the text is cut instead)
static void append(char *out, size_t size, int *n, const char *fmt, ...)
{
    if (*n >= (int)size - 1) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out + *n, size - *n, fmt, ap);
    va_end(ap);
    *n = r < 0 ? *n : *n + r >= (int)size ? (int)size - 1 : *n + r;
}

static void send_status(int64_t chat_id)
{
    static char out[1024];   // up to GOVEE_MAX sensor lines; only the bot task calls this
    char span[24];
    fmt_span(uptime_s(), span, sizeof(span));
    int n = snprintf(out, sizeof(out), "\xE2\x9C\x85 %s\n%s %s\n", T->st_online, T->st_uptime, span);

    wifi_ap_record_t ap;
    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        n += snprintf(out + n, sizeof(out) - n, "Wi-Fi: %s (%s %d dBm)\nIP: " IPSTR "\n",
                      (const char *)ap.ssid, T->st_signal, ap.rssi, IP2STR(&ip.ip));
    }

    weather_t w;
    if (net_get_weather(&w)) {
        bool f = config_get_u8("fahr");
        float temp = f ? w.temp * 9.0f / 5.0f + 32.0f : w.temp;
        n += snprintf(out + n, sizeof(out) - n, "%s %s: %.1f\xC2\xB0%c, %s\n", T->st_weather, g_cfg.city,
                      temp, f ? 'F' : 'C', ui_weather_text(w.code));
    } else {
        n += snprintf(out + n, sizeof(out) - n, "%s: %s\n", T->st_weather, T->st_no_weather);
    }
    static govee_sensor_t sens[GOVEE_MAX];
    int ns = govee_get(sens, GOVEE_MAX, true);
    bool f = config_get_u8("fahr");
    float k = f ? 9.0f / 5.0f : 1.0f, off = f ? 32.0f : 0.0f;
    for (int i = 0; i < ns; i++) {
        const govee_sensor_t *g = &sens[i];
        const char *arrow = !g->has_trend ? "" : g->trend > 0 ? " \xE2\x86\x91" : g->trend < 0 ? " \xE2\x86\x93" : " \xE2\x86\x92";
        append(out, sizeof(out), &n, "\xF0\x9F\x8C\xA1 %s: %.1f\xC2\xB0%c%s", g->name, g->temp_c * k + off,
               f ? 'F' : 'C', arrow);
        if (g->humidity >= 0) {
            append(out, sizeof(out), &n, ", %.0f%%", g->humidity);
        }
        append(out, sizeof(out), &n, "\n    ");
        append(out, sizeof(out), &n, T->sens_minmax_fmt, g->day_min * k + off, g->day_max * k + off);
        if (g->changed_at) {
            struct tm tm;
            localtime_r(&g->changed_at, &tm);
            char when[12];
            clock_fmt(when, sizeof(when), tm.tm_hour, tm.tm_min);
            append(out, sizeof(out), &n, " \xC2\xB7 ");
            append(out, sizeof(out), &n, T->sens_updated_fmt, when);
        }
        if (!g->changed_at) {
            append(out, sizeof(out), &n, "\n    \xE2\x8F\xB3 %s", T->sens_wait);   // value of unknown age
        } else if (!g->shown) {
            append(out, sizeof(out), &n, "\n    \xE2\x9A\xA0 %s", T->st_sens_gone);
        }
        append(out, sizeof(out), &n, "\n");
    }
    // free / lowest since boot / largest block (an HTTPS connection needs a big one)
    append(out, sizeof(out), &n, "%s: %u KB (min %u, block %u)\nTinySkyBoard v%s", T->st_memory,
           (unsigned)(esp_get_free_heap_size() / 1024), (unsigned)(esp_get_minimum_free_heap_size() / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024), esp_app_get_description()->version);
    reply(chat_id, out);
}
// Cleans the text and checks it fits the screen (in any rotation) and the byte limit.
// Replies to the user and returns false if it can't be shown.
static bool prepare_text(int64_t chat_id, const char *text, char *out, size_t out_size)
{
    static char clean[CLEAN_MAX];
    int dropped = sanitize(text, clean, sizeof(clean));
    if (!clean[0]) {
        reply(chat_id, T->tg_no_glyphs);
        return false;
    }
    int chars = 0;
    for (const char *p = clean; *p; p++) {
        chars += ((unsigned char)*p & 0xc0) != 0x80;
    }
    int max_chars = 0;
    bool fits = strlen(clean) < out_size;
    lvgl_port_lock(0);
    if (!ui_text_fits(clean, &max_chars)) {
        fits = false;
    } else if (!fits) {
        max_chars = chars * (out_size - 1) / strlen(clean);   // only the byte limit was exceeded
    }
    lvgl_port_unlock();
    if (!fits) {
        char msg[200];
        snprintf(msg, sizeof(msg), T->tg_too_long_fmt, chars, max_chars);
        reply(chat_id, msg);
        return false;
    }
    strlcpy(out, clean, out_size);
    if (dropped) {
        reply(chat_id, T->tg_some_dropped);
    }
    return true;
}

// ---------- /agendar assistant ----------

typedef enum { W_IDLE, W_TEXT, W_WHEN, W_DUR, W_REPEAT } wizard_step_t;

static wizard_step_t s_step;
static sched_t s_draft;
static time_t s_step_time;

static void ask_when(int64_t chat_id)
{
    const char *const btns[] = {T->btn_now, T->btn_in30};
    send_kb(chat_id, T->sch_ask_when, btns, 2, 2);
}

static void ask_duration(int64_t chat_id)
{
    const char *const btns[] = {"15 min", "30 min", "1 h", "2 h", "4 h", T->btn_day};
    send_kb(chat_id, T->sch_ask_dur, btns, 6, 3);
}

static void ask_repeat(int64_t chat_id)
{
    send_kb(chat_id, T->sch_ask_repeat, T->repeat, 4, 2);
}

static void wizard_input(int64_t chat_id, const char *text)
{
    time_t now = time(NULL);
    switch (s_step) {
    case W_TEXT:
        if (!prepare_text(chat_id, text, s_draft.text, sizeof(s_draft.text))) {
            return;   // keep waiting for the text
        }
        s_step = W_WHEN;
        ask_when(chat_id);
        break;

    case W_WHEN: {
        time_t start;
        int h, m;
        if (!parse_when(text, now, &start, &h, &m)) {
            reply(chat_id, T->sch_bad_when);
            ask_when(chat_id);
            return;
        }
        if (start < now - 60) {
            reply(chat_id, T->sch_past);
            ask_when(chat_id);
            return;
        }
        s_draft.start = start;
        s_draft.hour = h;
        s_draft.minute = m;
        s_step = W_DUR;
        ask_duration(chat_id);
        break;
    }

    case W_DUR: {
        int min = parse_duration(text);
        if (min <= 0 || min > 7 * 1440) {
            reply(chat_id, T->sch_bad_dur);
            ask_duration(chat_id);
            return;
        }
        s_draft.dur_min = min;
        s_step = W_REPEAT;
        ask_repeat(chat_id);
        break;
    }

    case W_REPEAT: {
        int rep = -1;
        for (int l = 0; l < LANG_COUNT && rep < 0; l++) {
            for (int i = 0; i < 4; i++) {
                if (strcasecmp(text, i18n_get(l)->repeat[i]) == 0) {
                    rep = i;
                }
            }
        }
        if (rep < 0) {
            ask_repeat(chat_id);
            return;
        }
        s_draft.repeat = rep;
        if (rep != REPEAT_ONCE && s_draft.dur_min > 1440) {
            s_draft.dur_min = 1440;   // a daily repeat lasts at most one day
        }
        s_step = W_IDLE;
        if (!sched_add(&s_draft)) {
            reply(chat_id, T->sch_full);
            return;
        }
        char line[80], out[SCHED_TEXT + 140];
        fmt_entry(&s_draft, line, sizeof(line));
        snprintf(out, sizeof(out), "%s\n%s\n\xE2\x80\x9C%s\xE2\x80\x9D", T->sch_done, line, s_draft.text);
        reply(chat_id, out);
        break;
    }

    default:
        break;
    }
    s_step_time = now;
}

// ---------- commands ----------

// /mensagens: what is on the panel right now and whether it was opened with the button
static void send_messages(int64_t chat_id)
{
    static msg_item_t items[MSG_ITEMS_MAX];
    static char out[1536];
    int n_items = msg_active(items, MSG_ITEMS_MAX);
    if (n_items == 0) {
        reply(chat_id, T->msg_none);
        return;
    }
    int n = 0;
    append(out, sizeof(out), &n, "%s\n", T->tg_msgs_title);
    time_t now = time(NULL);
    struct tm today;
    localtime_r(&now, &today);
    for (int i = 0; i < n_items; i++) {
        const msg_item_t *m = &items[i];
        // first ~50 characters, cut on a UTF-8 boundary
        char text[160];
        int bytes = 0, chars = 0;
        while (m->text[bytes] && bytes < (int)sizeof(text) - 4) {
            if (((unsigned char)m->text[bytes] & 0xc0) != 0x80 && chars++ == 50) {
                break;
            }
            bytes++;
        }
        snprintf(text, sizeof(text), "%.*s%s", bytes, m->text, m->text[bytes] ? "\xE2\x80\xA6" : "");
        for (char *p = text; *p; p++) {
            if (*p == '\n') {
                *p = ' ';
            }
        }
        char when[24] = "";
        if (m->when > 0) {
            struct tm t;
            localtime_r(&m->when, &t);
            char hm[12];
            clock_fmt(hm, sizeof(hm), t.tm_hour, t.tm_min);
            if (t.tm_yday == today.tm_yday && t.tm_year == today.tm_year) {
                snprintf(when, sizeof(when), " \xC2\xB7 %s", hm);
            } else {
                snprintf(when, sizeof(when), " \xC2\xB7 %02d/%02d %s", t.tm_mday % 32, (t.tm_mon + 1) % 13, hm);
            }
        }
        // 🔵 unread / ✅ read; ✉ direct / 🔔 scheduled
        append(out, sizeof(out), &n, "\n%d. %s %s %s\n    %s%s\n", i + 1, m->unread ? "\xF0\x9F\x94\xB5" : "\xE2\x9C\x85",
               m->scheduled ? "\xF0\x9F\x94\x94" : "\xE2\x9C\x89\xEF\xB8\x8F", m->unread ? T->tg_msg_unread : T->tg_msg_read,
               text, when);
    }
    reply(chat_id, out);
}

static void handle_command(int64_t chat_id, const char *text)
{
    static const char *const HELP[] = {"/ajuda", "/help", "/ayuda", "/start"};
    static const char *const CLEAR[] = {"/limpar", "/clear", "/borrar"};
    static const char *const SCHEDULE[] = {"/agendar", "/schedule", "/programar"};
    static const char *const LIST[] = {"/agenda", "/list", "/lista"};
    static const char *const CANCEL[] = {"/cancelar", "/cancel"};
    static const char *const STATUS[] = {"/status", "/estado", "/ping"};
    static const char *const MESSAGES[] = {"/mensagens", "/messages", "/mensajes", "/msgs"};
    static const char *const RAIN[] = {"/chuva", "/rain", "/lluvia"};

    char cmd[32];
    strlcpy(cmd, text, sizeof(cmd));
    cmd[strcspn(cmd, " @")] = '\0';   // strip arguments and the "@BotName"
    lower(cmd);
    const char *arg = strchr(text, ' ');
    arg = arg ? arg + strspn(arg, " ") : "";

    bool in_wizard = s_step != W_IDLE;
    s_step = W_IDLE;   // any command leaves the assistant

    if (is_cmd(cmd, HELP, 4)) {
        reply(chat_id, T->tg_help);
    } else if (is_cmd(cmd, STATUS, 3)) {
        send_status(chat_id);
    } else if (is_cmd(cmd, MESSAGES, 4)) {
        send_messages(chat_id);
    } else if (is_cmd(cmd, RAIN, 3)) {
        // "/chuva" toggles; "/chuva on|off" (or sim/não, si/no) sets it
        bool on = !config_get_u8("rain_al");
        if (arg[0]) {
            on = strchr("1osSyY", arg[0]) && strncasecmp(arg, "off", 3) != 0;
        }
        config_set_u8("rain_al", on);
        reply(chat_id, on ? T->tg_rain_on : T->tg_rain_off);
    } else if (is_cmd(cmd, CLEAR, 3)) {
        msg_clear_all();
        reply(chat_id, T->tg_cleared);
    } else if (is_cmd(cmd, SCHEDULE, 3)) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        if (tm.tm_year < (2024 - 1900)) {
            reply(chat_id, T->sch_no_clock);
            return;
        }
        memset(&s_draft, 0, sizeof(s_draft));
        s_step = W_TEXT;
        s_step_time = now;
        reply(chat_id, T->sch_ask_text);
    } else if (is_cmd(cmd, LIST, 3)) {
        send_list(chat_id);
    } else if (is_cmd(cmd, CANCEL, 2)) {
        if (!arg[0]) {
            reply(chat_id, in_wizard ? T->sch_aborted : T->sch_bad_index);
            return;
        }
        int n = atoi(arg);
        if (n < 1 || !sched_remove(n - 1)) {
            reply(chat_id, T->sch_bad_index);
            return;
        }
        char out[64];
        snprintf(out, sizeof(out), T->sch_removed_fmt, n);
        reply(chat_id, out);
        send_list(chat_id);
    } else {
        reply(chat_id, T->tg_help);
    }
}

static void handle_message(const cJSON *msg)
{
    int64_t chat_id = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetObjectItem(msg, "chat"), "id"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "text"));
    if (!chat_id) {
        return;
    }

    if (!g_cfg.tg_owner) {
        g_cfg.tg_owner = chat_id;
        config_save();
        ESP_LOGI(TAG, "new owner registered");
        reply(chat_id, T->tg_welcome);
        reply(chat_id, T->tg_help);
        if (!text || text[0] == '/') {
            return;
        }
    } else if (chat_id != g_cfg.tg_owner) {
        ESP_LOGW(TAG, "ignored a message from another chat");
        reply(chat_id, T->tg_private);
        return;
    }

    if (!text) {
        reply(chat_id, T->tg_only_text);
        return;
    }
    if (text[0] == '/') {
        handle_command(chat_id, text);
        return;
    }
    if (s_step != W_IDLE && time(NULL) - s_step_time < WIZARD_IDLE_S) {
        wizard_input(chat_id, text);
        return;
    }
    s_step = W_IDLE;

    char clean[MSG_TEXT];
    if (!prepare_text(chat_id, text, clean, sizeof(clean))) {
        return;
    }
    ESP_LOGI(TAG, "message: %s", clean);
    int n = msg_add_direct(clean);
    if (n > 1) {
        char out[96];
        snprintf(out, sizeof(out), T->tg_ok_count_fmt, n);
        reply(chat_id, out);
    } else {
        reply(chat_id, T->tg_ok);
    }
}

static volatile tg_state_t s_state = TG_OFF;

// Alerts from other tasks (sensors, rain) go out from this task, between polls: one TLS
// connection at a time, the memory is tight
#define NOTE_MAX    240
#define NOTE_QUEUE  6
static QueueHandle_t s_notes;

void telegram_notify(const char *text)
{
    if (!s_notes || !g_cfg.tg_owner) {
        return;
    }
    char note[NOTE_MAX];
    strlcpy(note, text, sizeof(note));
    if (xQueueSend(s_notes, note, 0) != pdTRUE) {
        ESP_LOGW(TAG, "alert queue full, dropped: %s", note);
    }
}

tg_state_t telegram_state(void)
{
    return s_state;
}

static void telegram_task(void *arg)
{
    s_state = TG_CONNECTING;
    set_commands();

    bool announced = false;
    int64_t fail_since = 0;     // uptime when the connection dropped (0 = ok)
    time_t fail_wall = 0;       // wall time when it dropped, for the notice
    int64_t offset = 0;
    char url[256];
    while (true) {
        char note[NOTE_MAX];
        while (announced && !fail_since && xQueueReceive(s_notes, note, 0) == pdTRUE) {
            ESP_LOGI(TAG, "alert: %s", note);
            reply(g_cfg.tg_owner, note);
        }
        // the first poll (and the first after an outage) returns at once, so the notice goes out quickly;
        // a shorter wait while alerts are queued
        int wait_s = (!announced || fail_since) ? 0 : uxQueueMessagesWaiting(s_notes) ? 1 : POLL_TIMEOUT_S;
        snprintf(url, sizeof(url),
                 "https://api.telegram.org/bot%s/getUpdates?timeout=%d&limit=5&offset=%" PRId64,
                 g_cfg.tg_token, wait_s, offset);
        cJSON *root = net_http_json(url, NULL, (wait_s + 10) * 1000);
        if (!cJSON_IsTrue(cJSON_GetObjectItem(root, "ok"))) {
            cJSON_Delete(root);
            s_state = TG_FAILING;
            if (!fail_since) {
                fail_since = uptime_s();
                fail_wall = time(NULL);
                ESP_LOGW(TAG, "no connection to Telegram");
            }
            vTaskDelay(pdMS_TO_TICKS(10 * 1000));   // no network or wrong token
            continue;
        }

        s_state = TG_OK;
        // connection ok: report the boot once, and outages longer than 1 min
        if (g_cfg.tg_owner && !announced) {
            announce_boot();
            announced = true;
        } else if (g_cfg.tg_owner && fail_since && uptime_s() - fail_since >= OUTAGE_MIN_S) {
            char span[24], since[24], out[160];
            fmt_span(uptime_s() - fail_since, span, sizeof(span));
            fmt_since(fail_wall, since, sizeof(since));
            snprintf(out, sizeof(out), T->st_back_fmt, span, since);
            ESP_LOGI(TAG, "%s", out);
            reply(g_cfg.tg_owner, out);
        }
        fail_since = 0;
        save_alive();

        const cJSON *upd;
        cJSON_ArrayForEach(upd, cJSON_GetObjectItem(root, "result")) {
            offset = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(upd, "update_id")) + 1;
            const cJSON *msg = cJSON_GetObjectItem(upd, "message");
            if (msg) {
                handle_message(msg);
            }
        }
        cJSON_Delete(root);
    }
}

void telegram_start(void)
{
    nvs_handle_t nvs;
    if (nvs_open("painel", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_i64(nvs, "alive", &s_prev_alive);
        nvs_close(nvs);
    }
    s_notes = xQueueCreate(NOTE_QUEUE, NOTE_MAX);
    ESP_LOGI(TAG, "bot active, owner %" PRId64, g_cfg.tg_owner);
    xTaskCreate(telegram_task, "telegram", 10240, NULL, 3, NULL);
}
