#include "app.h"
#include "config.h"
#include "i18n.h"
#include "board.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
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

const char *portal_help_html(uint8_t lang);   // portal_help.c

#define PORTAL_TIMEOUT_MS   (15 * 60 * 1000)   // nothing saved: restart and try the Wi-Fi again
#define MAX_NETS            20

static const char *TAG = "portal";
static char s_nets[MAX_NETS][33];
static httpd_handle_t s_server;
static esp_timer_handle_t s_lan_timer;   // home-network setup closes by itself
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
    char out[160];
    size_t o = 0;
    for (; *s && o < sizeof(out) - 7; s++) {
        const char *rep = NULL;
        switch (*s) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        }
        if (rep) {
            o += strlcpy(out + o, rep, sizeof(out) - o);
        } else {
            out[o++] = *s;
        }
    }
    out[o] = '\0';
    httpd_resp_sendstr_chunk(req, out);
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
    ".rot{display:grid;grid-template-columns:1fr 1fr;gap:8px}"
    "label.ro{display:flex;flex-direction:column;align-items:center;gap:4px;margin:0;padding:8px;"
    "border:1px solid #263049;border-radius:10px;color:#e8ecf4;font-size:.85em;cursor:pointer}"
    "label.ro input{display:none}label.ro.sel{border-color:#4fc3f7;background:#14243a}"
    "nav{display:flex;gap:10px;align-items:center}nav a{color:#8a94a8;text-decoration:none}"
    "nav .l{text-transform:uppercase}nav a.on{color:#4fc3f7;font-weight:600}"
    ".ver{text-align:center;color:#8a94a8;font-size:.8em;margin-top:20px}"
    "nav .h{margin-right:auto;border:1px solid #263049;border-radius:8px;padding:4px 10px;color:#e8ecf4}"
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

// Page header + bar with the help/back link and the languages
static void send_head(httpd_req_t *req, uint8_t lang, bool help)
{
    const i18n_t *L = i18n_get(lang);
    char link[96];
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    snprintf(link, sizeof(link), "<nav><a class=h href='%s?lang=%s'>%s %s</a>", help ? "/" : "/ajuda",
             L->code, help ? "&larr;" : "?", help ? L->p_back_link : L->p_help_link);
    httpd_resp_sendstr_chunk(req, link);
    for (uint8_t i = 0; i < LANG_COUNT; i++) {
        snprintf(link, sizeof(link), "<a class='l%s' href='%s?lang=%s'>%s</a>", i == lang ? " on" : "",
                 help ? "/ajuda" : "/", i18n_get(i)->code, i18n_get(i)->code);
        httpd_resp_sendstr_chunk(req, link);
    }
    httpd_resp_sendstr_chunk(req, "</nav>");
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
    httpd_resp_sendstr_chunk(req, html);
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
    httpd_resp_sendstr_chunk(req, "<label>");
    httpd_resp_sendstr_chunk(req, L->p_rotation);
    httpd_resp_sendstr_chunk(req, "</label><div class=rot>");
    for (int i = 0; i < 4; i++) {
        char html[200];
        snprintf(html, sizeof(html),
                 "<label class='ro%s'><input type=radio name=usb value=%d%s onchange=pick(this)>"
                 "<svg data-u=%d></svg>%s</label>",
                 i == cur ? " sel" : "", i, i == cur ? " checked" : "", i, L->p_usb[i]);
        httpd_resp_sendstr_chunk(req, html);
    }
    httpd_resp_sendstr_chunk(req, "</div><small>");
    httpd_resp_sendstr_chunk(req, L->p_btn_legend);
    httpd_resp_sendstr_chunk(req, "<br>");
    httpd_resp_sendstr_chunk(req, L->p_rotation_hint);
    httpd_resp_sendstr_chunk(req, "</small>");
    // drawing: upright board with USB at the bottom, rotated clockwise; the text always stays upright
    httpd_resp_sendstr_chunk(req,
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
    send_head(req, lang, true);
    httpd_resp_sendstr_chunk(req, portal_help_html(lang));
    httpd_resp_sendstr_chunk(req, "</body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t root_get(httpd_req_t *req)
{
    uint8_t lang = page_lang(req);
    const i18n_t *L = i18n_get(lang);
    send_head(req, lang, false);
    httpd_resp_sendstr_chunk(req, "<h1>");
    httpd_resp_sendstr_chunk(req, L->p_title);
    httpd_resp_sendstr_chunk(req, "</h1><form method=post action=/save><label>");
    httpd_resp_sendstr_chunk(req, L->p_network);
    httpd_resp_sendstr_chunk(req, "</label><input name=ssid list=nets required autocomplete=off value=\"");
    send_escaped(req, g_cfg.ssid);
    httpd_resp_sendstr_chunk(req, "\"><datalist id=nets>");
    for (int i = 0; i < s_n_nets; i++) {
        httpd_resp_sendstr_chunk(req, "<option value=\"");
        send_escaped(req, s_nets[i]);
        httpd_resp_sendstr_chunk(req, "\">");
    }
    httpd_resp_sendstr_chunk(req, "</datalist><small>");
    httpd_resp_sendstr_chunk(req, L->p_network_hint);
    httpd_resp_sendstr_chunk(req, "</small><label>");
    httpd_resp_sendstr_chunk(req, L->p_password);
    httpd_resp_sendstr_chunk(req, "</label><input name=pass type=password autocomplete=off><small>");
    httpd_resp_sendstr_chunk(req, L->p_password_hint);
    httpd_resp_sendstr_chunk(req, "</small><label>");
    httpd_resp_sendstr_chunk(req, L->p_city);
    httpd_resp_sendstr_chunk(req, "</label><input name=city required value=\"");
    send_escaped(req, g_cfg.city);
    httpd_resp_sendstr_chunk(req, "\"><small>");
    httpd_resp_sendstr_chunk(req, L->p_city_hint);

    httpd_resp_sendstr_chunk(req, "</small><label>");
    httpd_resp_sendstr_chunk(req, L->p_language);
    httpd_resp_sendstr_chunk(req, "</label><select name=lang>");
    for (uint8_t i = 0; i < LANG_COUNT; i++) {
        char opt[64];
        snprintf(opt, sizeof(opt), "<option value=%s%s>%s", i18n_get(i)->code,
                 i == lang ? " selected" : "", i18n_get(i)->name);
        httpd_resp_sendstr_chunk(req, opt);
    }

    httpd_resp_sendstr_chunk(req, "</select><label>");
    httpd_resp_sendstr_chunk(req, L->p_unit);
    httpd_resp_sendstr_chunk(req, "</label><select name=unit>");
    bool fahr = config_get_u8("fahr");
    httpd_resp_sendstr_chunk(req, fahr ? "<option value=c>Celsius (°C)<option value=f selected>Fahrenheit (°F)"
                                       : "<option value=c selected>Celsius (°C)<option value=f>Fahrenheit (°F)");
    httpd_resp_sendstr_chunk(req, "</select>");
    send_rotation(req, L);
    send_slider(req, "bl_day", L->p_bright_day, 5, config_get_u8_def("bl_day", DIM_DEFAULT_DAY));
    send_slider(req, "bl_night", L->p_bright_night, 1, config_get_u8_def("bl_night", DIM_DEFAULT_NIGHT));
    send_slider(req, "led_lvl", L->p_bright_led, 0, config_get_u8_def("led_lvl", DIM_DEFAULT_LED));
    send_slider(req, "led_night", L->p_bright_led_night, 0, config_get_u8_def("led_night", DIM_DEFAULT_LED_NIGHT));
    send_slider(req, "led_alert", L->p_bright_alert, 0, config_get_u8_def("led_alert", DIM_DEFAULT_ALERT));
    httpd_resp_sendstr_chunk(req, "<small>");
    httpd_resp_sendstr_chunk(req, L->p_bright_hint);
    httpd_resp_sendstr_chunk(req, "</small><label>");
    httpd_resp_sendstr_chunk(req, L->p_tg);
    httpd_resp_sendstr_chunk(req, "</label><input name=tg autocomplete=off placeholder=\"");
    httpd_resp_sendstr_chunk(req, g_cfg.tg_token[0] ? L->p_tg_set : "123456789:ABC...");
    httpd_resp_sendstr_chunk(req, "\"><small>");
    httpd_resp_sendstr_chunk(req, L->p_tg_hint);
    httpd_resp_sendstr_chunk(req, "</small><button>");
    httpd_resp_sendstr_chunk(req, L->p_save);
    char footer[96];
    snprintf(footer, sizeof(footer), "</button></form><p class=ver>TinySkyBoard v%s</p></body></html>",
             esp_app_get_description()->version);
    httpd_resp_sendstr_chunk(req, footer);
    return httpd_resp_sendstr_chunk(req, NULL);
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

static bool form_value(const char *body, const char *key, char *out, size_t size)
{
    if (httpd_query_key_value(body, key, out, size) != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    url_decode(out);
    return true;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((uint32_t)arg));
    esp_restart();
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[768];
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

    char ssid[33], pass[65], city[48], unit[4], tg[80], lang[4], num[8];
    form_value(body, "ssid", ssid, sizeof(ssid));
    form_value(body, "pass", pass, sizeof(pass));
    form_value(body, "city", city, sizeof(city));
    form_value(body, "unit", unit, sizeof(unit));
    form_value(body, "tg", tg, sizeof(tg));
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
    config_save();
    config_set_u8("fahr", unit[0] == 'f');
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
    for (uint8_t i = 0; i < LANG_COUNT; i++) {
        if (strcmp(lang, i18n_get(i)->code) == 0) {
            config_set_u8("lang", i);
            i18n_set(i);   // the "saved" page already uses the new language
        }
    }
    ESP_LOGI(TAG, "saved: network '%s', city '%s'", g_cfg.ssid, g_cfg.city);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<h1>");
    httpd_resp_sendstr_chunk(req, T->p_saved_title);
    httpd_resp_sendstr_chunk(req, "</h1><p>");
    httpd_resp_sendstr_chunk(req, T->p_saved_pre);
    send_escaped(req, g_cfg.ssid);
    httpd_resp_sendstr_chunk(req, T->p_saved_post);
    httpd_resp_sendstr_chunk(req, "</p></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);

    if (lvgl_port_lock(0)) {
        ui_config_message(T->saved_restarting);
        lvgl_port_unlock();
    }
    xTaskCreate(restart_task, "restart", 2048, (void *)1500, 5, NULL);
    return ESP_OK;
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
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
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
    if (captive) {
        httpd_register_uri_handler(server, &other);
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
