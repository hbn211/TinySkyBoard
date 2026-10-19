#include "app.h"
#include "config.h"
#include "i18n.h"
#include "board.h"
#include "schedule.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include <inttypes.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_lvgl_port.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "cJSON.h"

const char *portal_help_html(uint8_t lang);   // portal_help.c

#define PORTAL_TIMEOUT_MS   (15 * 60 * 1000)   // nothing saved: restart and try the Wi-Fi again
#define MAX_NETS            20
#define CLEAN_TEXT_MAX      1024   // a scheduled text from the page, before cleaning

static const char *TAG = "portal";

// Pages are built from many small pieces; sending each one as its own chunk is slow over the home
// network (each waits for the Wi-Fi) and the server gave up halfway. They're gathered here and sent
// in 4 KB chunks. out(req, NULL) sends the rest and ends the response. One request at a time.
static char s_out[4096];
static size_t s_out_len;
static httpd_req_t *s_out_req;

static esp_err_t out(httpd_req_t *req, const char *str)
{
    if (req != s_out_req) {   // a new request: drop anything left from one that failed halfway
        s_out_req = req;
        s_out_len = 0;
    }
    esp_err_t err = ESP_OK;
    size_t n = str ? strlen(str) : 0;
    if (!str || s_out_len + n > sizeof(s_out)) {
        if (s_out_len) {
            err = httpd_resp_send_chunk(req, s_out, s_out_len);
            s_out_len = 0;
        }
    }
    if (!str) {
        esp_err_t end = httpd_resp_send_chunk(req, NULL, 0);
        return err != ESP_OK ? err : end;
    }
    if (n >= sizeof(s_out)) {   // bigger than the buffer (e.g. the help page): straight through
        return httpd_resp_send_chunk(req, str, n);
    }
    memcpy(s_out + s_out_len, str, n);
    s_out_len += n;
    return err;
}
static char s_nets[MAX_NETS][33];
static httpd_handle_t s_server;
static esp_timer_handle_t s_lan_timer;   // home-network setup closes by itself
static bool s_captive;   // own Wi-Fi (setup mode): no /agenda (no clock, no message store)
static int s_n_nets;

// ---------- DNS: answers 192.168.4.1 for any name (captive portal) ----------

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS: socket failed");
        vTaskDelete(NULL);
    }

    uint8_t buf[512];
    while (true) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n < 12) {
            continue;
        }
        // skip the question name, then QTYPE + QCLASS
        int p = 12;
        while (p < n && buf[p]) {
            p += buf[p] + 1;
        }
        p += 5;
        if (p > n || p + 16 > (int)sizeof(buf)) {
            continue;
        }
        buf[2] = 0x81;                  // response, recursion desired
        buf[3] = 0x80;                  // recursion available, no error
        buf[6] = 0; buf[7] = 1;         // 1 answer
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        const uint8_t answer[] = {
            0xc0, 0x0c,                 // pointer to the question name
            0x00, 0x01, 0x00, 0x01,     // type A, class IN
            0x00, 0x00, 0x00, 0x3c,     // TTL 60 s
            0x00, 0x04, 192, 168, 4, 1,
        };
        memcpy(buf + p, answer, sizeof(answer));
        sendto(sock, buf, p + sizeof(answer), 0, (struct sockaddr *)&from, from_len);
    }
}

// ---------- HTTP ----------

static void send_escaped(httpd_req_t *req, const char *s)
{
    char esc[160];
    size_t o = 0;
    for (; *s && o < sizeof(esc) - 7; s++) {
        const char *rep = NULL;
        switch (*s) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        }
        if (rep) {
            o += strlcpy(esc + o, rep, sizeof(esc) - o);
        } else {
            esc[o++] = *s;
        }
    }
    esc[o] = '\0';
    out(req, esc);
}

static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>TinySkyBoard</title><style>"
    "body{font-family:system-ui,sans-serif;background:#0b0f1a;color:#e8ecf4;max-width:420px;margin:0 auto;padding:16px}"
    "h1{font-size:1.3em}label{display:block;margin:16px 0 4px;color:#8a94a8;font-size:.9em}"
    "input,select{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #263049;"
    "background:#161d2e;color:#fff;font-size:1em}"
    "button{margin-top:24px;width:100%;padding:12px;border:0;border-radius:8px;background:#4fc3f7;"
    "color:#0b0f1a;font-size:1em;font-weight:600}small{color:#8a94a8;display:block;margin-top:4px}"
    "input[type=range]{padding:0;accent-color:#4fc3f7}.rv{float:right;color:#e8ecf4}"
    "label.chk{display:flex;align-items:center;gap:10px;color:#e8ecf4}"
    "label.chk input{width:22px;height:22px;flex:none;accent-color:#4fc3f7}"
    ".hrs{display:flex;align-items:center;gap:8px}.hrs span{color:#8a94a8}.hrs input{color-scheme:dark}"
    ".days{display:flex;flex-wrap:wrap;gap:4px 12px;margin-top:8px}.days label{display:flex;align-items:center;"
    "gap:4px;margin:0;color:#e8ecf4}.days input{width:18px;height:18px;accent-color:#4fc3f7}"
    ".al{margin:10px 0}.al b{display:block;margin-bottom:4px;font-weight:500}"
    ".al4{display:grid;grid-template-columns:repeat(4,1fr);gap:6px}.al4 input{padding:8px 6px;font-size:.9em}"
    ".rot{display:grid;grid-template-columns:1fr 1fr;gap:8px}"
    "label.ro{display:flex;flex-direction:column;align-items:center;gap:4px;margin:0;padding:8px;"
    "border:1px solid #263049;border-radius:10px;color:#e8ecf4;font-size:.85em;cursor:pointer}"
    "label.ro input{display:none}label.ro.sel{border-color:#4fc3f7;background:#14243a}"
    "nav{display:flex;gap:10px;align-items:center}nav a{color:#8a94a8;text-decoration:none}"
    "nav .l{text-transform:uppercase}nav a.on{color:#4fc3f7;font-weight:600}"
    ".ver{text-align:center;color:#8a94a8;font-size:.8em;margin-top:20px}"
    "h2{font-size:.8em;font-weight:600;letter-spacing:.08em;text-transform:uppercase;color:#4fc3f7;"
    "margin:28px 0 -4px;padding-bottom:6px;border-bottom:1px solid #263049}"
    "@media(min-width:900px){body:has(form){max-width:1040px;padding:24px}"
    "form{columns:2;column-gap:20px}section{break-inside:avoid;display:inline-block;width:100%;"
    "box-sizing:border-box;margin-bottom:20px;padding:4px 18px 18px;border:1px solid #263049;"
    "border-radius:14px;background:#0f1524}h2{margin-top:14px}"
    "form button{column-span:all;display:block;max-width:420px;margin:4px auto 0}}"
    "nav .h{margin-right:auto;border:1px solid #263049;border-radius:8px;padding:4px 10px;color:#e8ecf4}"
    "textarea{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #263049;"
    "background:#161d2e;color:#fff;font:inherit;resize:vertical}"
    "form.sc{margin:14px 0;padding:12px;border:1px solid #263049;border-radius:12px;background:#0f1524}"
    "form.sc h2{margin:0 0 10px}form.sc .hrs{margin-top:8px}form.sc .hrs span{white-space:nowrap}"
    ".btns{display:flex;gap:8px}.btns button{margin-top:12px}button.del{background:#3a1d27;color:#ff8a80}"
    ".st{display:inline-block;margin-bottom:8px;font-size:.8em;color:#4fc3f7}"
    ".ok{color:#81c784}.err{color:#ff8a80}a.ag{display:block;margin-top:14px;padding:12px;border:1px solid #263049;"
    "border-radius:10px;color:#4fc3f7;text-decoration:none;text-align:center}"
    "</style></head><body>";

// ?lang=xx only changes the page language; what counts is what gets saved in the form
static uint8_t page_lang(httpd_req_t *req)
{
    uint8_t lang = config_get_u8_def("lang", LANG_EN);
    char query[24], code[4];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "lang", code, sizeof(code)) == ESP_OK) {
        for (uint8_t i = 0; i < LANG_COUNT; i++) {
            if (strcmp(code, i18n_get(i)->code) == 0) {
                lang = i;
            }
        }
    }
    return lang;
}

// Page header + bar with the help/back link and the languages; self = this page's path
static void send_head(httpd_req_t *req, uint8_t lang, const char *self)
{
    const i18n_t *L = i18n_get(lang);
    bool setup = strcmp(self, "/") == 0;
    char link[96];
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    out(req, PAGE_HEAD);
    snprintf(link, sizeof(link), "<nav><a class=h href='%s?lang=%s'>%s %s</a>", setup ? "/ajuda" : "/",
             L->code, setup ? "?" : "&larr;", setup ? L->p_help_link : L->p_back_link);
    out(req, link);
    for (uint8_t i = 0; i < LANG_COUNT; i++) {
        snprintf(link, sizeof(link), "<a class='l%s' href='%s?lang=%s'>%s</a>", i == lang ? " on" : "",
                 self, i18n_get(i)->code, i18n_get(i)->code);
        out(req, link);
    }
    out(req, "</nav>");
}

// Brightness slider; moving it applies to the board right away (/preview), before saving
static void send_slider(httpd_req_t *req, const char *name, const char *label, int min, int value)
{
    char html[400];
    snprintf(html, sizeof(html),
             "<label>%s <span class=rv id=%s_v>%d%%</span></label>"
             "<input type=range name=%s min=%d max=100 value=%d "
             "oninput=\"document.getElementById('%s_v').textContent=this.value+'%%';"
             "fetch('/preview?%s='+this.value)\">",
             label, name, value, name, min, value, name, name);
    out(req, html);
}

// Time slider (screen cycle in s, sensor interval in min); no live preview: it only matters over time
static void send_time(httpd_req_t *req, const char *name, const char *label, int min, int max, int def,
                      const char *unit)
{
    int value = config_get_u8_def(name, def);
    char html[320];
    snprintf(html, sizeof(html),
             "<label>%s <span class=rv id=%s_v>%d %s</span></label>"
             "<input type=range name=%s min=%d max=%d value=%d "
             "oninput=\"document.getElementById('%s_v').textContent=this.value+' %s'\">",
             label, name, value, unit, name, min, max, value, name, unit);
    out(req, html);
}

static void send_seconds(httpd_req_t *req, const char *name, const char *label)
{
    send_time(req, name, label, CYCLE_MIN_S, CYCLE_MAX_S, CYCLE_DEFAULT_S, "s");
}

static bool form_value(const char *body, const char *key, char *out, size_t size);
static void send_backup(httpd_req_t *req, const i18n_t *L);

// Sensor alerts: one row per sensor (known now or saved before), 4 limits each; blank = no limit.
// Temperatures in the panel's unit, saved in tenths of a degree Celsius.
static void send_alerts(httpd_req_t *req, const i18n_t *L)
{
    static govee_alert_t saved[GOVEE_MAX];
    static govee_ident_t list[GOVEE_MAX];
    int ns = govee_alerts_load(saved, GOVEE_MAX);
    int n = govee_known(list, GOVEE_MAX);
    for (int i = 0; i < ns && n < GOVEE_MAX; i++) {   // saved ones the panel hasn't seen (yet)
        bool found = false;
        for (int j = 0; j < n; j++) {
            found |= strcmp(list[j].device, saved[i].device) == 0;
        }
        if (!found) {
            strlcpy(list[n].device, saved[i].device, sizeof(list[n].device));
            strlcpy(list[n].name, saved[i].name, sizeof(list[n].name));
            n++;
        }
    }
    bool f = config_get_u8("fahr");
    out(req, "<label>");
    out(req, L->p_alerts);
    out(req, "</label><small>");
    out(req, L->p_alerts_hint);
    out(req, "</small>");
    if (n == 0) {
        out(req, "<small>");
        out(req, L->p_alerts_none);
        out(req, "</small>");
    }
    static const char *const FIELD[] = {"tn", "tx", "hn", "hx"};
    for (int i = 0; i < n; i++) {
        const govee_alert_t *a = NULL;
        for (int k = 0; k < ns; k++) {
            if (strcmp(saved[k].device, list[i].device) == 0) {
                a = &saved[k];
            }
        }
        char html[220], val[4][12] = {"", "", "", ""};
        if (a) {
            if (a->tmin != GOVEE_T_NONE) {
                snprintf(val[0], sizeof(val[0]), "%.1f", f ? a->tmin / 10.0 * 9 / 5 + 32 : a->tmin / 10.0);
            }
            if (a->tmax != GOVEE_T_NONE) {
                snprintf(val[1], sizeof(val[1]), "%.1f", f ? a->tmax / 10.0 * 9 / 5 + 32 : a->tmax / 10.0);
            }
            if (a->hmin != GOVEE_H_NONE) {
                snprintf(val[2], sizeof(val[2]), "%d", a->hmin);
            }
            if (a->hmax != GOVEE_H_NONE) {
                snprintf(val[3], sizeof(val[3]), "%d", a->hmax);
            }
        }
        out(req, "<div class=al><b>");
        send_escaped(req, list[i].name);
        snprintf(html, sizeof(html), "</b><input type=hidden name=a%did value=\"", i);
        out(req, html);
        send_escaped(req, list[i].device);
        snprintf(html, sizeof(html), "\"><input type=hidden name=a%dnm value=\"", i);
        out(req, html);
        send_escaped(req, list[i].name);
        out(req, "\"><div class=al4>");
        const char *ph[] = {L->p_al_tmin, L->p_al_tmax, L->p_al_hmin, L->p_al_hmax};
        for (int k = 0; k < 4; k++) {
            snprintf(html, sizeof(html),
                     "<input name=a%d%s type=number step=%s inputmode=decimal placeholder=\"%s %s\" value=\"%s\">", i,
                     FIELD[k], k < 2 ? "0.1" : "1", ph[k], k < 2 ? (f ? "\xC2\xB0" "F" : "\xC2\xB0" "C") : "%", val[k]);
            out(req, html);
        }
        out(req, "</div></div>");
    }
    out(req, config_get_u8("galert_q") ? "<label class=chk><input type=checkbox name=galert_q value=1 checked> "
                                      : "<label class=chk><input type=checkbox name=galert_q value=1> ");
    out(req, L->p_al_silent);
    out(req, "</label>");
}

static void save_alerts(const char *body)
{
    static govee_alert_t al[GOVEE_MAX];
    static const char *const FIELD[] = {"tn", "tx", "hn", "hx"};
    bool f = config_get_u8("fahr");
    int n = 0;
    for (int i = 0; i < GOVEE_MAX; i++) {
        char key[12], v[48];
        snprintf(key, sizeof(key), "a%did", i);
        if (!form_value(body, key, v, sizeof(v)) || !v[0]) {
            continue;
        }
        govee_alert_t a = {.tmin = GOVEE_T_NONE, .tmax = GOVEE_T_NONE, .hmin = GOVEE_H_NONE, .hmax = GOVEE_H_NONE};
        strlcpy(a.device, v, sizeof(a.device));
        snprintf(key, sizeof(key), "a%dnm", i);
        form_value(body, key, a.name, sizeof(a.name));
        bool any = false;
        for (int k = 0; k < 4; k++) {
            snprintf(key, sizeof(key), "a%d%s", i, FIELD[k]);
            if (!form_value(body, key, v, sizeof(v)) || !v[0]) {
                continue;
            }
            float x = strtof(v, NULL);
            any = true;
            if (k < 2) {
                float c = f ? (x - 32) * 5 / 9 : x;
                int16_t t = (int16_t)(c * 10 + (c >= 0 ? 0.5f : -0.5f));
                if (k == 0) {
                    a.tmin = t;
                } else {
                    a.tmax = t;
                }
            } else {
                int h = (int)(x + 0.5f);
                h = h < 0 ? 0 : h > 100 ? 100 : h;
                if (k == 2) {
                    a.hmin = h;
                } else {
                    a.hmax = h;
                }
            }
        }
        if (any) {
            al[n++] = a;
        }
    }
    govee_alerts_save(al, n);
    char on[4];
    config_set_u8("galert_q", form_value(body, "galert_q", on, sizeof(on)));
}

// Sensor comparisons: up to GOVEE_CMP_MAX pairs "A x B", each picked from the sensors on the account
static void send_compare(httpd_req_t *req, const i18n_t *L)
{
    static govee_ident_t list[GOVEE_MAX + 2 * GOVEE_CMP_MAX];
    static govee_cmp_t cmp[GOVEE_CMP_MAX];
    govee_cmp_load(cmp);
    int n = govee_known(list, GOVEE_MAX);
    for (int i = 0; i < GOVEE_CMP_MAX; i++) {   // saved ones the panel hasn't seen (yet): listed by ID
        const char *ids[] = {cmp[i].a, cmp[i].b};
        for (int k = 0; k < 2; k++) {
            bool found = !ids[k][0];
            for (int j = 0; j < n && !found; j++) {
                found = strcmp(list[j].device, ids[k]) == 0;
            }
            if (!found) {
                strlcpy(list[n].device, ids[k], sizeof(list[n].device));
                strlcpy(list[n].name, ids[k], sizeof(list[n].name));
                n++;
            }
        }
    }
    out(req, "<label>");
    out(req, L->p_cmp);
    out(req, "</label><small>");
    out(req, L->p_cmp_hint);
    out(req, "</small>");
    if (n < 2) {
        out(req, "<small>");
        out(req, L->p_alerts_none);
        out(req, "</small>");
        return;
    }
    for (int i = 0; i < GOVEE_CMP_MAX; i++) {
        out(req, "<div class=hrs>");
        const char *cur[] = {cmp[i].a, cmp[i].b};
        for (int k = 0; k < 2; k++) {
            char html[64];
            snprintf(html, sizeof(html), "%s<select name=c%d%c><option value=''>&mdash;", k ? "<span>&times;</span>" : "",
                     i, k ? 'b' : 'a');
            out(req, html);
            for (int j = 0; j < n; j++) {
                out(req, "<option value=\"");
                send_escaped(req, list[j].device);
                out(req, strcmp(list[j].device, cur[k]) == 0 ? "\" selected>" : "\">");
                send_escaped(req, list[j].name);
            }
            out(req, "</select>");
        }
        out(req, "</div>");
    }
}

static void save_compare(const char *body)
{
    static govee_cmp_t cmp[GOVEE_CMP_MAX];
    memset(cmp, 0, sizeof(cmp));
    for (int i = 0; i < GOVEE_CMP_MAX; i++) {
        char key[8];
        snprintf(key, sizeof(key), "c%da", i);
        form_value(body, key, cmp[i].a, sizeof(cmp[i].a));
        snprintf(key, sizeof(key), "c%db", i);
        form_value(body, key, cmp[i].b, sizeof(cmp[i].b));
        if (!cmp[i].a[0] || !cmp[i].b[0] || strcmp(cmp[i].a, cmp[i].b) == 0) {
            cmp[i].a[0] = cmp[i].b[0] = '\0';   // incomplete or the same sensor twice
        }
    }
    govee_cmp_save(cmp);
}

// From/to time fields (minutes of the day); the value is always "HH:MM", the phone shows its own picker
static void send_hours(httpd_req_t *req, const char *const names[2], const int cur[2])
{
    for (int k = 0; k < 2; k++) {
        char html[80];
        snprintf(html, sizeof(html), "%s<input type=time name=%s value=%02d:%02d>", k ? "<span>&ndash;</span>" : "",
                 names[k], cur[k] / 60 % 24, cur[k] % 60);
        out(req, html);
    }
}

// "HH:MM" (or just "HH") -> minutes of the day; -1 if invalid
static int parse_hhmm(const char *s)
{
    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) < 1 || h < 0 || h > 23 || m < 0 || m > 59) {
        return -1;
    }
    return h * 60 + m;
}

// "Work" page: city, hours and days of the week
static void send_work(httpd_req_t *req, const i18n_t *L)
{
    out(req, "<label>");
    out(req, L->p_work);
    out(req, "</label><input name=work_city maxlength=46 value=\"");
    send_escaped(req, g_cfg.work_city);
    out(req, "\"><label>");
    out(req, L->p_work_hours);
    out(req, "</label><div class=hrs>");
    static const char *const NAMES[] = {"work_from", "work_to"};
    const int cur[] = {g_cfg.work_from, g_cfg.work_to};
    send_hours(req, NAMES, cur);
    out(req, "</div><div class=days>");
    for (int d = 0; d < 7; d++) {
        char html[96];
        snprintf(html, sizeof(html), "<label><input type=checkbox name=wd%d value=1%s>%s</label>", d,
                 g_cfg.work_days & (1 << d) ? " checked" : "", L->days[d]);
        out(req, html);
    }
    out(req, "</div><small>");
    out(req, L->p_work_hint);
    out(req, "</small>");
}

// Night mode: checkbox + from/to hours
static void send_night(httpd_req_t *req, const i18n_t *L)
{
    out(req, config_get_u8("night_on") ? "<label class=chk><input type=checkbox name=night_on value=1 checked> "
                                      : "<label class=chk><input type=checkbox name=night_on value=1> ");
    out(req, L->p_night_on);
    out(req, "</label><label>");
    out(req, L->p_night_hours);
    out(req, "</label><div class=hrs>");
    static const char *const NAMES[] = {"night_from", "night_to"};
    const int cur[] = {config_get_u8_def("night_from", NIGHT_DEFAULT_FROM) % 24 * 60 + config_get_u8("night_fm") % 60,
                       config_get_u8_def("night_to", NIGHT_DEFAULT_TO) % 24 * 60 + config_get_u8("night_tm") % 60};
    send_hours(req, NAMES, cur);
    out(req, "</div><small>");
    out(req, L->p_night_hint);
    out(req, "</small>");
}

// Board position picker: 4 drawings (USB, C/R buttons and the screen with upright text).
// Tapping one rotates the screen right away (/preview?usb=N) to check before saving.
static void send_rotation(httpd_req_t *req, const i18n_t *L)
{
    uint8_t rot = config_get_u8("rot") % 4;
    int cur = 0;
    for (int i = 0; i < 4; i++) {
        if (BOARD_USB_TO_ROT[i] == rot) {
            cur = i;
        }
    }
    out(req, "<label>");
    out(req, L->p_rotation);
    out(req, "</label><div class=rot>");
    for (int i = 0; i < 4; i++) {
        char html[200];
        snprintf(html, sizeof(html),
                 "<label class='ro%s'><input type=radio name=usb value=%d%s onchange=pick(this)>"
                 "<svg data-u=%d></svg>%s</label>",
                 i == cur ? " sel" : "", i, i == cur ? " checked" : "", i, L->p_usb[i]);
        out(req, html);
    }
    out(req, "</div><small>");
    out(req, L->p_btn_legend);
    out(req, "<br>");
    out(req, L->p_rotation_hint);
    out(req, "</small>");
    // drawing: upright board with USB at the bottom, rotated clockwise; the text always stays upright
    out(req,
        "<script>"
        "function pick(e){document.querySelectorAll('label.ro').forEach(function(l){l.classList.remove('sel')});"
        "e.parentNode.classList.add('sel');fetch('/preview?usb='+e.value)}"
        "document.querySelectorAll('svg[data-u]').forEach(function(v){"
        "var u=+v.dataset.u,a=u*90,l=u%2,W=l?120:84,H=l?84:120,cx=W/2,cy=H/2,r=a*Math.PI/180,"
        "c=Math.cos(r),s=Math.sin(r);"
        "function P(x,y){return 'x='+(cx+x*c-y*s).toFixed(1)+' y='+(cy+x*s+y*c).toFixed(1)}"
        "function T(x,y,t,f,z){return '<text '+P(x,y)+' fill='+f+' font-size='+z+' font-weight=700 "
        "text-anchor=middle dominant-baseline=central>'+t+'</text>'}"
        "v.setAttribute('viewBox','0 0 '+W+' '+H);v.setAttribute('width',W);v.setAttribute('height',H);"
        "v.innerHTML='<g transform=\"translate('+cx+' '+cy+') rotate('+a+')\">"
        "<rect x=-26 y=-52 width=52 height=104 rx=6 fill=#1d2a44 stroke=#3a4a6b />"
        "<rect x=-21 y=-46 width=42 height=78 rx=3 fill=#0b0f1a stroke=#4fc3f7 stroke-width=.8 />"
        "<rect x=-8 y=50 width=16 height=6 rx=2 fill=#9aa4b8 />"
        "<circle cx=-14 cy=43 r=5 fill=#4fc3f7 /><circle cx=14 cy=43 r=5 fill=#ff6b6b /></g>'"
        "+T(0,-7,'12:34','#fff',12)+T(-14,43,'C','#0b0f1a',7)+T(14,43,'R','#0b0f1a',7)})"
        "</script>");
}

// Live preview while dragging: applies without saving
static esp_err_t preview_get(httpd_req_t *req)
{
    char query[32], val[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "bl_day", val, sizeof(val)) == ESP_OK ||
            httpd_query_key_value(query, "bl_night", val, sizeof(val)) == ESP_OK) {
            board_backlight_set(atoi(val));
        } else if (httpd_query_key_value(query, "usb", val, sizeof(val)) == ESP_OK) {
            int u = atoi(val);
            if (u >= 0 && u < 4 && lvgl_port_lock(0)) {
                board_display_set_rotation((lv_display_rotation_t)BOARD_USB_TO_ROT[u]);
                ui_rebuild();
                lvgl_port_unlock();
            }
        } else if (httpd_query_key_value(query, "led_alert", val, sizeof(val)) == ESP_OK) {
            led_set_alert_level(atoi(val), true);   // flashes white so you see the level
        } else if (httpd_query_key_value(query, "led_lvl", val, sizeof(val)) == ESP_OK ||
                   httpd_query_key_value(query, "led_night", val, sizeof(val)) == ESP_OK) {
            led_set_level(atoi(val));
        }
    }
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t help_get(httpd_req_t *req)
{
    uint8_t lang = page_lang(req);
    send_head(req, lang, "/ajuda");
    out(req, portal_help_html(lang));
    out(req, "</body></html>");
    return out(req, NULL);
}

// Starts section n (closing the previous one): a titled group, a card in two columns on wide screens
static void section(httpd_req_t *req, const i18n_t *L, int n)
{
    out(req, n ? "</section><section><h2>" : "<section><h2>");
    out(req, L->p_sec[n]);
    out(req, "</h2>");
}

static esp_err_t root_get(httpd_req_t *req)
{
    uint8_t lang = page_lang(req);
    const i18n_t *L = i18n_get(lang);
    send_head(req, lang, "/");
    out(req, "<h1>");
    out(req, L->p_title);
    out(req, "</h1>");
    if (!s_captive) {
        char link[48];
        snprintf(link, sizeof(link), "<a class=ag href='/agenda?lang=%s'>", L->code);
        out(req, link);
        out(req, L->p_sched_link);
        out(req, " &rarr;</a>");
    }
    out(req, "<form method=post action=/save>");
    section(req, L, 0);
    out(req, "<label>");
    out(req, L->p_network);
    out(req, "</label><input name=ssid list=nets required autocomplete=off value=\"");
    send_escaped(req, g_cfg.ssid);
    out(req, "\"><datalist id=nets>");
    for (int i = 0; i < s_n_nets; i++) {
        out(req, "<option value=\"");
        send_escaped(req, s_nets[i]);
        out(req, "\">");
    }
    out(req, "</datalist><small>");
    out(req, L->p_network_hint);
    out(req, "</small><label>");
    out(req, L->p_password);
    out(req, "</label><input name=pass type=password autocomplete=off><small>");
    out(req, L->p_password_hint);
    out(req, "</small><label>");
    out(req, L->p_city);
    out(req, "</label><input name=city required value=\"");
    send_escaped(req, g_cfg.city);
    out(req, "\"><small>");
    out(req, L->p_city_hint);
    out(req, "</small><label>");
    out(req, L->p_cities);
    out(req, "</label><input name=cities maxlength=120 placeholder=\"Florian&oacute;polis, Tokyo\" value=\"");
    send_escaped(req, g_cfg.cities);
    out(req, "\"><small>");
    out(req, L->p_cities_hint);
    out(req, "</small>");
    section(req, L, 1);
    out(req, "<label>");
    out(req, L->p_language);
    out(req, "</label><select name=lang>");
    for (uint8_t i = 0; i < LANG_COUNT; i++) {
        char opt[64];
        snprintf(opt, sizeof(opt), "<option value=%s%s>%s", i18n_get(i)->code,
                 i == lang ? " selected" : "", i18n_get(i)->name);
        out(req, opt);
    }

    out(req, "</select><label>");
    out(req, L->p_unit);
    out(req, "</label><select name=unit>");
    bool fahr = config_get_u8("fahr");
    out(req, fahr ? "<option value=c>Celsius (°C)<option value=f selected>Fahrenheit (°F)"
                                       : "<option value=c selected>Celsius (°C)<option value=f>Fahrenheit (°F)");
    out(req, "</select>");
    out(req, "<label>");
    out(req, L->p_clock);
    out(req, "</label><select name=h12>");
    out(req, g_cfg.h12 ? "<option value=0>24 h (14:05)<option value=1 selected>12 h (2:05 PM)"
                                            : "<option value=0 selected>24 h (14:05)<option value=1>12 h (2:05 PM)");
    out(req, "</select>");
    send_night(req, L);
    send_rotation(req, L);
    section(req, L, 2);
    send_slider(req, "bl_day", L->p_bright_day, 5, config_get_u8_def("bl_day", DIM_DEFAULT_DAY));
    send_slider(req, "bl_night", L->p_bright_night, 1, config_get_u8_def("bl_night", DIM_DEFAULT_NIGHT));
    send_slider(req, "led_lvl", L->p_bright_led, 0, config_get_u8_def("led_lvl", DIM_DEFAULT_LED));
    send_slider(req, "led_night", L->p_bright_led_night, 0, config_get_u8_def("led_night", DIM_DEFAULT_LED_NIGHT));
    send_slider(req, "led_alert", L->p_bright_alert, 0, config_get_u8_def("led_alert", DIM_DEFAULT_ALERT));
    out(req, "<small>");
    out(req, L->p_bright_hint);
    out(req, "</small>");
    section(req, L, 3);
    send_seconds(req, "t_dash", L->p_t_dash);
    out(req, config_get_u8_def("wx_on", 1) ? "<label class=chk><input type=checkbox name=wx_on value=1 checked> "
                                           : "<label class=chk><input type=checkbox name=wx_on value=1> ");
    out(req, L->p_wx_on);
    out(req, "</label>");
    send_seconds(req, "t_wx", L->p_t_wx);
    send_work(req, L);
    send_seconds(req, "t_city", L->p_t_city);
    send_seconds(req, "t_sens", L->p_t_sens);
    send_seconds(req, "t_msg", L->p_t_msg);
    out(req, "<small>");
    out(req, L->p_t_hint);
    out(req, "</small>");
    section(req, L, 4);
    out(req, "<label>");
    out(req, L->p_tg);
    out(req, "</label><input name=tg autocomplete=off placeholder=\"");
    out(req, g_cfg.tg_token[0] ? L->p_tg_set : "123456789:ABC...");
    out(req, "\"><small>");
    out(req, L->p_tg_hint);
    out(req, "</small><label>");
    out(req, L->p_govee);
    out(req, "</label><input name=govee autocomplete=off placeholder=\"");
    out(req, g_cfg.govee_key[0] ? L->p_tg_set : "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx");
    out(req, "\"><small>");
    out(req, L->p_govee_hint);
    out(req, "</small>");
    send_time(req, "govee_int", L->p_govee_int, 1, 60, GOVEE_DEFAULT_MIN, "min");
    out(req, "<small>");
    out(req, L->p_govee_int_hint);
    out(req, "</small>");
    send_alerts(req, L);
    send_compare(req, L);
    out(req, "</section><button>");
    out(req, L->p_save);
    char footer[96];
    out(req, "</button></form>");
    send_backup(req, L);
    snprintf(footer, sizeof(footer), "<p class=ver>TinySkyBoard v%s</p></body></html>", esp_app_get_description()->version);
    out(req, footer);
    return out(req, NULL);
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

// The value arrives URL-encoded and can be ~3x longer than decoded (a Govee ID "AA:BB:..." of 23
// characters is 37 with %3A): read it into a large buffer, decode, then copy (one request at a time)
static bool form_value(const char *body, const char *key, char *out, size_t size)
{
    static char raw[2048];
    if (httpd_query_key_value(body, key, raw, sizeof(raw)) != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    url_decode(raw);
    strlcpy(out, raw, size);
    return true;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((uint32_t)arg));
    esp_restart();
}

static esp_err_t save_post(httpd_req_t *req)
{
    static char body[2560];   // static: the server task has a small stack (one request at a time)
    int len = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';

    char ssid[33], pass[65], city[48], unit[4], tg[80], govee[48], lang[4], num[8];
    form_value(body, "ssid", ssid, sizeof(ssid));
    form_value(body, "pass", pass, sizeof(pass));
    form_value(body, "city", city, sizeof(city));
    form_value(body, "unit", unit, sizeof(unit));
    form_value(body, "tg", tg, sizeof(tg));
    form_value(body, "govee", govee, sizeof(govee));
    char work[sizeof(g_cfg.work_city)];
    if (form_value(body, "work_city", work, sizeof(work))) {
        strlcpy(g_cfg.work_city, work, sizeof(g_cfg.work_city));
    }
    int t;
    if (form_value(body, "work_from", num, sizeof(num)) && (t = parse_hhmm(num)) >= 0) {
        g_cfg.work_from = t;
    }
    if (form_value(body, "work_to", num, sizeof(num)) && (t = parse_hhmm(num)) >= 0) {
        g_cfg.work_to = t;
    }
    g_cfg.work_days = 0;
    for (int d = 0; d < 7; d++) {
        char key[6];
        snprintf(key, sizeof(key), "wd%d", d);
        if (form_value(body, key, num, sizeof(num))) {
            g_cfg.work_days |= 1 << d;
        }
    }
    char cities[sizeof(g_cfg.cities)];
    if (form_value(body, "cities", cities, sizeof(cities))) {
        strlcpy(g_cfg.cities, cities, sizeof(g_cfg.cities));
    }
    form_value(body, "lang", lang, sizeof(lang));

    if (!ssid[0] || !city[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, T->p_required);
        return ESP_FAIL;
    }
    if (pass[0] || strcmp(ssid, g_cfg.ssid) != 0) {
        strlcpy(g_cfg.pass, pass, sizeof(g_cfg.pass));
    }
    strlcpy(g_cfg.ssid, ssid, sizeof(g_cfg.ssid));
    if (strcasecmp(city, g_cfg.city) != 0) {
        strlcpy(g_cfg.city, city, sizeof(g_cfg.city));
        g_cfg.lat[0] = g_cfg.lon[0] = '\0';   // geocoding on the next boot
    }
    if (strcmp(tg, "0") == 0) {
        g_cfg.tg_token[0] = '\0';
        g_cfg.tg_owner = 0;
    } else if (tg[0] && strcmp(tg, g_cfg.tg_token) != 0) {
        strlcpy(g_cfg.tg_token, tg, sizeof(g_cfg.tg_token));
        g_cfg.tg_owner = 0;   // new bot, new owner
    }
    // pasted keys often come with spaces around them
    char *g = govee + strspn(govee, " \t");
    for (char *e = g + strlen(g); e > g && (e[-1] == ' ' || e[-1] == '\t'); *--e = '\0') {
    }
    memmove(govee, g, strlen(g) + 1);
    if (strcmp(govee, "0") == 0) {
        g_cfg.govee_key[0] = '\0';
    } else if (govee[0]) {
        strlcpy(g_cfg.govee_key, govee, sizeof(g_cfg.govee_key));
    }
    config_split_city();
    config_save();
    config_set_u8("fahr", unit[0] == 'f');
    if (form_value(body, "h12", num, sizeof(num))) {
        config_set_u8("h12", num[0] == '1');
    }
    if (form_value(body, "usb", num, sizeof(num))) {
        int u = atoi(num);
        if (u >= 0 && u < 4) {
            config_set_u8("rot", BOARD_USB_TO_ROT[u]);
        }
    }
    static const char *const BRIGHT[] = {"bl_day", "bl_night", "led_lvl", "led_night", "led_alert"};
    for (int i = 0; i < 5; i++) {
        if (form_value(body, BRIGHT[i], num, sizeof(num))) {
            int v = atoi(num);
            config_set_u8(BRIGHT[i], v < 0 ? 0 : v > 100 ? 100 : v);
        }
    }
    if (form_value(body, "govee_int", num, sizeof(num))) {
        int v = atoi(num);
        config_set_u8("govee_int", v < 1 ? 1 : v > 60 ? 60 : v);
    }
    config_set_u8("wx_on", form_value(body, "wx_on", num, sizeof(num)));   // unchecked boxes aren't sent
    config_set_u8("night_on", form_value(body, "night_on", num, sizeof(num)));
    save_alerts(body);
    save_compare(body);
    static const char *const NIGHT[][2] = {{"night_from", "night_fm"}, {"night_to", "night_tm"}};
    for (int i = 0; i < 2; i++) {
        int t;
        if (form_value(body, NIGHT[i][0], num, sizeof(num)) && (t = parse_hhmm(num)) >= 0) {
            config_set_u8(NIGHT[i][0], t / 60);
            config_set_u8(NIGHT[i][1], t % 60);
        }
    }
    static const char *const TIMES[] = {"t_dash", "t_wx", "t_city", "t_sens", "t_msg"};
    for (int i = 0; i < 5; i++) {
        if (form_value(body, TIMES[i], num, sizeof(num))) {
            int v = atoi(num);
            config_set_u8(TIMES[i], v < CYCLE_MIN_S ? CYCLE_MIN_S : v > CYCLE_MAX_S ? CYCLE_MAX_S : v);
        }
    }
    for (uint8_t i = 0; i < LANG_COUNT; i++) {
        if (strcmp(lang, i18n_get(i)->code) == 0) {
            config_set_u8("lang", i);
            i18n_set(i);   // the "saved" page already uses the new language
        }
    }
    ESP_LOGI(TAG, "saved");

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    out(req, PAGE_HEAD);
    out(req, "<h1>");
    out(req, T->p_saved_title);
    out(req, "</h1><p>");
    out(req, T->p_saved_pre);
    send_escaped(req, g_cfg.ssid);
    out(req, T->p_saved_post);
    out(req, "</p></body></html>");
    out(req, NULL);

    if (lvgl_port_lock(0)) {
        ui_config_message(T->saved_restarting);
        lvgl_port_unlock();
    }
    xTaskCreate(restart_task, "restart", 2048, (void *)1500, 5, NULL);
    return ESP_OK;
}

// ---------- /agenda: scheduled messages (home-network setup only: needs the clock and the message store) ----------

static const int DURS[] = {15, 30, 60, 120, 240, 480, 1440};

static void dur_label(int min, char *out, size_t size)
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

// one card: an existing schedule (s->id) or the empty "new" one
static void send_sched_form(httpd_req_t *req, const i18n_t *L, const sched_t *s)
{
    char html[160];
    snprintf(html, sizeof(html), "<form method=post action=/agenda class=sc><input type=hidden name=id value=%" PRIu32
             "><input type=hidden name=lang value=%s>", s->id, L->code);
    out(req, html);
    if (s->id) {
        int st = sched_state(s->id);
        const char *tag = st == SCHED_ON_SCREEN ? L->sch_on_now : st == SCHED_DISMISSED ? L->sch_dismissed :
                          st == SCHED_SKIPPED ? L->sch_skipped : NULL;
        if (tag) {
            out(req, "<span class=st>");
            out(req, tag);
            out(req, "</span>");
        }
    } else {
        out(req, "<h2>");
        out(req, L->p_sched_new);
        out(req, "</h2>");
    }
    out(req, "<textarea name=text rows=2 maxlength=119 required placeholder=\"");
    out(req, L->p_sched_text);
    out(req, "\">");
    send_escaped(req, s->text);
    out(req, "</textarea><div class=hrs><select name=rep onchange=rp(this)>");
    for (int r = 0; r < 4; r++) {
        snprintf(html, sizeof(html), "<option value=%d%s>%s", r, r == s->repeat ? " selected" : "", L->repeat[r]);
        out(req, html);
    }
    // date of a one-time message (the start; repeats only use the time)
    time_t start = s->repeat == REPEAT_ONCE && s->start ? (time_t)s->start : time(NULL);
    struct tm t;
    localtime_r(&start, &t);
    int hour = s->repeat == REPEAT_ONCE && s->start ? t.tm_hour : s->hour, min = s->repeat == REPEAT_ONCE && s->start ? t.tm_min : s->minute;
    snprintf(html, sizeof(html), "</select><input type=date name=date value=%04d-%02d-%02d%s><input type=time name=time value=%02d:%02d required>",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, s->repeat == REPEAT_ONCE ? "" : " hidden", hour, min);
    out(req, html);
    out(req, "</div><div class=hrs><span>");
    out(req, L->p_sched_dur);
    out(req, "</span><select name=dur>");
    bool listed = false;
    for (int i = 0; i < (int)(sizeof(DURS) / sizeof(DURS[0])); i++) {
        listed |= DURS[i] == s->dur_min;
    }
    for (int i = -1; i < (int)(sizeof(DURS) / sizeof(DURS[0])); i++) {
        int d = i < 0 ? s->dur_min : DURS[i];
        if (i < 0 && listed) {
            continue;   // the current value goes first only when it isn't one of the usual ones
        }
        char lbl[16];
        dur_label(d, lbl, sizeof(lbl));
        snprintf(html, sizeof(html), "<option value=%d%s>%s", d, d == s->dur_min ? " selected" : "", lbl);
        out(req, html);
    }
    out(req, "</select></div>");
    if (g_cfg.tg_token[0]) {
        out(req, s->flags & SCHED_F_TG_DONE ? "<label class=chk><input type=checkbox name=tg value=1 checked> "
                                            : "<label class=chk><input type=checkbox name=tg value=1> ");
        out(req, L->p_sched_tg);
        out(req, "</label>");
    }
    out(req, "<div class=btns><button name=act value=save>");
    out(req, s->id ? L->p_sched_save : L->p_sched_add);
    out(req, "</button>");
    if (s->id) {
        out(req, "<button name=act value=del class=del onclick=\"return confirm(this.dataset.q)\" data-q=\"");
        out(req, L->p_sched_del_q);
        out(req, "\">");
        out(req, L->p_sched_del);
        out(req, "</button>");
    }
    out(req, "</div></form>");
}

static void lan_touch(void)
{
    if (s_lan_timer) {   // still being used: the home-network setup stays open
        esp_timer_restart(s_lan_timer, (uint64_t)PORTAL_LAN_TIMEOUT_MS * 1000);
    }
}

static esp_err_t agenda_get(httpd_req_t *req)
{
    lan_touch();
    uint8_t lang = page_lang(req);
    const i18n_t *L = i18n_get(lang);
    send_head(req, lang, "/agenda");
    out(req, "<h1>");
    out(req, L->p_sched_title);
    out(req, "</h1>");
    char query[32], m[4];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "m", m, sizeof(m)) == ESP_OK) {
        out(req, "<p class=ok>");
        out(req, m[0] == 'd' ? L->p_sched_deleted : L->p_sched_saved);
        out(req, "</p>");
    }
    static sched_t list[SCHED_MAX];
    int n = sched_list(list);
    if (n == 0) {
        out(req, "<small>");
        out(req, L->sch_empty);
        out(req, "</small>");
    }
    for (int i = 0; i < n; i++) {
        send_sched_form(req, L, &list[i]);
    }
    if (n < SCHED_MAX) {
        sched_t blank = {.repeat = REPEAT_ONCE, .dur_min = 60};
        time_t soon = time(NULL) + 5 * 60;
        blank.start = soon - soon % 300;   // next 5 minutes
        send_sched_form(req, L, &blank);
    } else {
        out(req, "<small>");
        out(req, L->sch_full);
        out(req, "</small>");
    }
    out(req, "<script>function rp(s){s.form.date.hidden=s.value!='0'}</script></body></html>");
    return out(req, NULL);
}

// error page: the browser's back button keeps what was typed
static esp_err_t agenda_error(httpd_req_t *req, const i18n_t *L, const char *msg)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    out(req, PAGE_HEAD);
    out(req, "<p class=err>");
    out(req, msg);
    out(req, "</p><button onclick=history.back()>&larr; ");
    out(req, L->p_back_link);
    out(req, "</button></body></html>");
    return out(req, NULL);
}

static esp_err_t agenda_post(httpd_req_t *req)
{
    lan_touch();
    static char body[1536];
    int len = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';
    char num[12], act[8], date[12], hm[8];
    form_value(body, "lang", num, sizeof(num));
    const i18n_t *L = T;
    for (uint8_t i = 0; i < LANG_COUNT; i++) {
        if (strcmp(num, i18n_get(i)->code) == 0) {
            L = i18n_get(i);
        }
    }
    form_value(body, "id", num, sizeof(num));
    uint32_t id = strtoul(num, NULL, 10);
    form_value(body, "act", act, sizeof(act));

    char loc[40];
    snprintf(loc, sizeof(loc), "/agenda?lang=%s&m=%c", L->code, strcmp(act, "del") == 0 ? 'd' : 's');
    if (strcmp(act, "del") == 0) {
        sched_delete_id(id);
    } else {
        static char text[CLEAN_TEXT_MAX], clean[CLEAN_TEXT_MAX];
        form_value(body, "text", text, sizeof(text));
        for (char *p = text; *p; p++) {
            if (*p == '\r') {
                *p = ' ';   // textarea line breaks come as CR LF
            }
        }
        text_sanitize(text, clean, sizeof(clean));
        int max_chars = 0;
        bool fits = clean[0] && strlen(clean) < SCHED_TEXT;
        if (fits) {
            lvgl_port_lock(0);
            fits = ui_text_fits(clean, &max_chars);
            lvgl_port_unlock();
        }
        if (!clean[0]) {
            return agenda_error(req, L, L->tg_no_glyphs);
        }
        if (!fits) {
            return agenda_error(req, L, L->p_sched_long);
        }
        sched_t s = {0};
        strlcpy(s.text, clean, sizeof(s.text));
        form_value(body, "rep", num, sizeof(num));
        s.repeat = atoi(num) % 4;
        form_value(body, "dur", num, sizeof(num));
        int dur = atoi(num);
        s.dur_min = dur < 1 ? 60 : dur > 7 * 1440 ? 7 * 1440 : dur;
        if (s.repeat != REPEAT_ONCE && s.dur_min > 1440) {
            s.dur_min = 1440;   // a daily repeat lasts at most one day
        }
        form_value(body, "tg", num, sizeof(num));
        s.flags = num[0] == '1' ? SCHED_F_TG_DONE : 0;
        form_value(body, "time", hm, sizeof(hm));
        int t = parse_hhmm(hm);
        if (t < 0) {
            return agenda_error(req, L, L->sch_bad_when);
        }
        s.hour = t / 60;
        s.minute = t % 60;
        if (s.repeat == REPEAT_ONCE) {
            struct tm tm = {0};
            form_value(body, "date", date, sizeof(date));
            if (sscanf(date, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) {
                return agenda_error(req, L, L->sch_bad_when);
            }
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            tm.tm_hour = s.hour;
            tm.tm_min = s.minute;
            tm.tm_isdst = -1;
            s.start = mktime(&tm);
            if (s.start + s.dur_min * 60 <= time(NULL)) {
                return agenda_error(req, L, L->sch_past);   // would already be over
            }
        }
        bool ok = id ? sched_update(id, &s) : sched_add(&s);
        if (!ok) {
            return agenda_error(req, L, id ? L->sch_bad_when : L->sch_full);
        }
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, NULL, 0);
}

// ---------- configuration backup: /backup downloads it, /restore loads it (e.g. on a new board) ----------
// Every key of the "painel" namespace except the short-lived ones and the secrets (the file must be
// safe to download while the page is open on the home network); blobs as hex.

static bool backup_skip(const char *key)
{
    static const char *const SKIP[] = {"portal", "alive", "msgs", "unread", "sched_st", "pass", "tg_token", "govee_key"};
    for (int i = 0; i < (int)(sizeof(SKIP) / sizeof(SKIP[0])); i++) {
        if (strcmp(key, SKIP[i]) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t backup_get(httpd_req_t *req)
{
    lan_touch();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"tinyskyboard-config.json\"");
    char line[200];
    snprintf(line, sizeof(line), "{\"tinyskyboard\":1,\"version\":\"%s\",\"nvs\":{", esp_app_get_description()->version);
    out(req, line);
    nvs_handle_t nvs;
    if (nvs_open("painel", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_iterator_t it = NULL;
        bool first = true;
        for (esp_err_t e = nvs_entry_find(NVS_DEFAULT_PART_NAME, "painel", NVS_TYPE_ANY, &it); e == ESP_OK;
             e = nvs_entry_next(&it)) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            if (backup_skip(info.key)) {
                continue;
            }
            snprintf(line, sizeof(line), "%s\"%s\":", first ? "" : ",", info.key);
            if (info.type == NVS_TYPE_U8) {
                uint8_t v = 0;
                nvs_get_u8(nvs, info.key, &v);
                snprintf(line + strlen(line), sizeof(line) - strlen(line), "{\"u8\":%u}", v);
                out(req, line);
            } else if (info.type == NVS_TYPE_I64) {
                int64_t v = 0;
                nvs_get_i64(nvs, info.key, &v);
                snprintf(line + strlen(line), sizeof(line) - strlen(line), "{\"i64\":\"%" PRId64 "\"}", v);
                out(req, line);
            } else if (info.type == NVS_TYPE_STR) {
                char v[160] = "";
                size_t len = sizeof(v);
                nvs_get_str(nvs, info.key, v, &len);
                out(req, line);
                out(req, "{\"str\":\"");
                for (const char *p = v; *p; p++) {   // JSON string escaping
                    char c[8];
                    if (*p == '"' || *p == '\\') {
                        snprintf(c, sizeof(c), "\\%c", *p);
                    } else if ((unsigned char)*p < 0x20) {
                        snprintf(c, sizeof(c), "\\u%04x", *p);
                    } else {
                        c[0] = *p;
                        c[1] = '\0';
                    }
                    out(req, c);
                }
                out(req, "\"}");
            } else if (info.type == NVS_TYPE_BLOB) {
                size_t len = 0;
                nvs_get_blob(nvs, info.key, NULL, &len);
                uint8_t *v = malloc(len ? len : 1);
                if (!v) {
                    continue;
                }
                nvs_get_blob(nvs, info.key, v, &len);
                out(req, line);
                out(req, "{\"hex\":\"");
                for (size_t i = 0; i < len; i++) {
                    char h[3];
                    snprintf(h, sizeof(h), "%02x", v[i]);
                    out(req, h);
                }
                out(req, "\"}");
                free(v);
            } else {
                continue;
            }
            first = false;
        }
        nvs_release_iterator(it);
        nvs_close(nvs);
    }
    out(req, "}}");
    return out(req, NULL);
}

static int hex_val(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static esp_err_t restore_post(httpd_req_t *req)
{
    lan_touch();
    if (req->content_len == 0 || req->content_len > 16384) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, NULL);
        return ESP_FAIL;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
        return ESP_FAIL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            return ESP_FAIL;
        }
        got += r;
    }
    body[got] = '\0';
    cJSON *root = cJSON_Parse(body);
    free(body);
    const cJSON *keys = cJSON_GetObjectItem(root, "nvs");
    int n = 0;
    nvs_handle_t nvs;
    if (cJSON_GetObjectItem(root, "tinyskyboard") && cJSON_IsObject(keys) &&
        nvs_open("painel", NVS_READWRITE, &nvs) == ESP_OK) {
        const cJSON *k;
        cJSON_ArrayForEach(k, keys) {
            const char *key = k->string;
            const cJSON *v;
            if (!key || strlen(key) >= NVS_KEY_NAME_MAX_SIZE || backup_skip(key)) {
                continue;
            }
            if ((v = cJSON_GetObjectItem(k, "u8")) && cJSON_IsNumber(v)) {
                n += nvs_set_u8(nvs, key, (uint8_t)v->valuedouble) == ESP_OK;
            } else if ((v = cJSON_GetObjectItem(k, "i64")) && cJSON_IsString(v)) {
                n += nvs_set_i64(nvs, key, strtoll(v->valuestring, NULL, 10)) == ESP_OK;
            } else if ((v = cJSON_GetObjectItem(k, "str")) && cJSON_IsString(v)) {
                n += nvs_set_str(nvs, key, v->valuestring) == ESP_OK;
            } else if ((v = cJSON_GetObjectItem(k, "hex")) && cJSON_IsString(v)) {
                size_t len = strlen(v->valuestring) / 2;
                uint8_t *b = malloc(len ? len : 1);
                bool ok = b != NULL;
                for (size_t i = 0; ok && i < len; i++) {
                    int hi = hex_val(v->valuestring[2 * i]), lo = hex_val(v->valuestring[2 * i + 1]);
                    ok = hi >= 0 && lo >= 0;
                    if (ok) {
                        b[i] = hi << 4 | lo;
                    }
                }
                if (ok) {
                    n += nvs_set_blob(nvs, key, b, len) == ESP_OK;
                }
                free(b);
            }
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "restored %d settings", n);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    if (n == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, T->p_restore_bad);
    }
    config_load();   // the page shows the loaded values; Save applies them (and restarts)
    return httpd_resp_sendstr(req, T->p_restore_ok);
}

// the setup page's backup section: download, and load a file (sent by the script as plain text)
static void send_backup(httpd_req_t *req, const i18n_t *L)
{
    out(req, "<section><h2>");
    out(req, L->p_backup);
    out(req, "</h2><a class=ag href=/backup download>");
    out(req, L->p_backup_get);
    out(req, "</a><small>");
    out(req, L->p_backup_hint);
    out(req, "</small><label>");
    out(req, L->p_restore);
    out(req, "</label><input type=file accept=.json,application/json onchange=rs(this)><small id=rsm></small>"
             "<script>function rs(i){var f=i.files[0];if(!f)return;f.text().then(function(t){"
             "return fetch('/restore',{method:'POST',body:t})}).then(function(r){return r.text()})"
             ".then(function(m){document.getElementById('rsm').textContent=m;alert(m);location.reload()})}"
             "</script></section>");
}

// Any other URL (Android/iOS/Windows connectivity checks) goes to the page
static esp_err_t redirect_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" PORTAL_AP_IP "/");
    return httpd_resp_send(req, NULL, 0);
}

// captive = own Wi-Fi: any other URL is redirected to the page (phones open it by themselves)
static bool http_start(bool captive)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;   // the save handler parses the whole form
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 12;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        s_server = NULL;
        return false;
    }
    httpd_handle_t server = s_server;

    const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    const httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    const httpd_uri_t help = {.uri = "/ajuda", .method = HTTP_GET, .handler = help_get};
    const httpd_uri_t preview = {.uri = "/preview", .method = HTTP_GET, .handler = preview_get};
    const httpd_uri_t other = {.uri = "/*", .method = HTTP_GET, .handler = redirect_get};
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    httpd_register_uri_handler(server, &help);
    httpd_register_uri_handler(server, &preview);
    s_captive = captive;
    const httpd_uri_t backup = {.uri = "/backup", .method = HTTP_GET, .handler = backup_get};
    const httpd_uri_t restore = {.uri = "/restore", .method = HTTP_POST, .handler = restore_post};
    httpd_register_uri_handler(server, &backup);
    httpd_register_uri_handler(server, &restore);
    if (captive) {
        httpd_register_uri_handler(server, &other);
    } else {
        const httpd_uri_t agenda = {.uri = "/agenda", .method = HTTP_GET, .handler = agenda_get};
        const httpd_uri_t agenda_save = {.uri = "/agenda", .method = HTTP_POST, .handler = agenda_post};
        httpd_register_uri_handler(server, &agenda);
        httpd_register_uri_handler(server, &agenda_save);
    }
    return true;
}

// ---------- Wi-Fi ----------

static void scan_networks(void)
{
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
        return;
    }
    uint16_t n = MAX_NETS;
    wifi_ap_record_t recs[MAX_NETS];
    esp_wifi_scan_get_ap_records(&n, recs);   // already sorted by signal
    s_n_nets = 0;
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        bool dup = !ssid[0];
        for (int j = 0; j < s_n_nets && !dup; j++) {
            dup = strcmp(s_nets[j], ssid) == 0;
        }
        if (!dup) {
            strlcpy(s_nets[s_n_nets++], ssid, sizeof(s_nets[0]));
        }
    }
    ESP_LOGI(TAG, "%d networks found", s_n_nets);
}

void portal_start(void)
{
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    wifi_config_t ap = {
        .ap = {
            .ssid = PORTAL_AP_SSID,
            .ssid_len = strlen(PORTAL_AP_SSID),
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 3,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    led_solid(0xff8000);

    scan_networks();
    ESP_ERROR_CHECK(http_start(true) ? ESP_OK : ESP_FAIL);
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "portal up: Wi-Fi '%s', http://%s", PORTAL_AP_SSID, PORTAL_AP_IP);

    xTaskCreate(restart_task, "portal_timeout", 2048, (void *)PORTAL_TIMEOUT_MS, 1, NULL);
}

// ---------- setup on the home network (opened from the button) ----------

static void lan_timeout_task(void *arg)
{
    ESP_LOGI(TAG, "home-network setup timed out");
    portal_lan_stop();
    if (lvgl_port_lock(0)) {
        ui_lan_setup_close();
        lvgl_port_unlock();
    }
    vTaskDelete(NULL);
}

static void lan_timeout_cb(void *arg)
{
    xTaskCreate(lan_timeout_task, "lan_timeout", 3072, NULL, 3, NULL);   // httpd_stop doesn't belong in a timer callback
}

bool portal_lan_start(char *url, size_t size)
{
    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
    if (s_server || !ip.ip.addr) {
        return false;   // already open, or not connected
    }
    scan_networks();   // short blip on the connection
    if (!http_start(false)) {
        return false;
    }
    snprintf(url, size, "http://" IPSTR "/", IP2STR(&ip.ip));
    if (!s_lan_timer) {
        const esp_timer_create_args_t args = {.callback = lan_timeout_cb, .name = "lan_setup"};
        esp_timer_create(&args, &s_lan_timer);
    }
    esp_timer_start_once(s_lan_timer, (uint64_t)PORTAL_LAN_TIMEOUT_MS * 1000);
    ESP_LOGI(TAG, "home-network setup open at %s", url);
    return true;
}

void portal_lan_stop(void)
{
    if (s_lan_timer) {
        esp_timer_stop(s_lan_timer);
    }
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "home-network setup closed");
    }
}
