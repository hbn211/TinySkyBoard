#include "app.h"
#include "board.h"
#include "config.h"
#include "i18n.h"
#include "schedule.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif_sntp.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_lvgl_port.h"
#include "cJSON.h"

#define CONNECTED_BIT       BIT0
#define FIRST_CONNECT_MS    (45 * 1000)        // not connected by then: open the portal
#define WEATHER_PERIOD_MS   (10 * 60 * 1000)
#define WEATHER_RETRY_MS    (30 * 1000)
#define RESP_MAX            8192   // geocoding with 10 results exceeds 4 KB

static const char *TAG = "net";
static EventGroupHandle_t s_events;
static uint32_t s_weather_color;   // 0 = no weather yet
static weather_t s_last_weather;   // for /status
static bool s_has_weather;

static void set_status(const char *text)
{
    if (lvgl_port_lock(0)) {
        ui_set_status(text);
        lvgl_port_unlock();
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        led_solid(0xff0000);
        char msg[64];
        snprintf(msg, sizeof(msg), LV_SYMBOL_WARNING " %s", T->wifi_lost);
        set_status(msg);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, CONNECTED_BIT);
        if (s_weather_color) {
            led_solid(s_weather_color);
        } else {
            led_solid(0x00ff00);
        }
    }
}

static void wifi_init(void)
{
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    wifi_config_t wcfg = {
        .sta = {
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    strlcpy((char *)wcfg.sta.ssid, g_cfg.ssid, sizeof(wcfg.sta.ssid));
    strlcpy((char *)wcfg.sta.password, g_cfg.pass, sizeof(wcfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    // no modem sleep: the board runs on USB, and sleep made the setup page stall over the home network
    esp_wifi_set_ps(WIFI_PS_NONE);
    led_solid(0x0000ff);

    // just the network name: the signal bars next to it sweep while connecting
    char status[64];
    snprintf(status, sizeof(status), LV_SYMBOL_WIFI " %s", g_cfg.ssid);
    set_status(status);
}

typedef struct {
    char *buf;
    int len;
} resp_t;

static esp_err_t http_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA) {
        resp_t *r = e->user_data;
        int n = e->data_len;
        if (r->len + n > RESP_MAX - 1) {
            n = RESP_MAX - 1 - r->len;
        }
        memcpy(r->buf + r->len, e->data, n);
        r->len += n;
        r->buf[r->len] = '\0';
    }
    return ESP_OK;
}

cJSON *net_http_json(const char *url, const char *post_body, int timeout_ms)
{
    return net_http_json_hdr(url, post_body, NULL, NULL, timeout_ms, NULL);
}

cJSON *net_http_json_hdr(const char *url, const char *post_body, const char *hdr, const char *hdr_value,
                         int timeout_ms, int *status_out)
{
    resp_t resp = {.buf = calloc(1, RESP_MAX)};
    if (!resp.buf) {
        return NULL;
    }
    const esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = timeout_ms,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (hdr) {
        esp_http_client_set_header(client, hdr, hdr_value);
    }
    if (post_body) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_body, strlen(post_body));
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (status_out) {
        *status_out = err == ESP_OK ? status : -1;
    }
    if (err != ESP_OK || status != 200) {
        // host only: the Telegram URL carries the bot token
        const char *host = url + strlen("https://");
        ESP_LOGW(TAG, "%s %.*s failed: err=%s status=%d", post_body ? "POST" : "GET", (int)strcspn(host, "/"), host,
                 esp_err_to_name(err), status);
    }

    cJSON *root = NULL;
    if (err == ESP_OK && status == 200) {
        root = cJSON_Parse(resp.buf);
    }
    free(resp.buf);
    return root;
}

static double json_num(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : 0;
}

static double json_at(const cJSON *obj, const char *key, int i)
{
    const cJSON *item = cJSON_GetArrayItem(cJSON_GetObjectItem(obj, key), i);
    return cJSON_IsNumber(item) ? item->valuedouble : 0;
}

// "2026-10-13" -> day of week (0 = Sunday)
static int weekday_of(const char *date)
{
    struct tm tm = {.tm_hour = 12};
    if (!date || sscanf(date, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3) {
        return 0;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    mktime(&tm);
    return tm.tm_wday;
}

void net_url_encode(const char *in, char *out, size_t size)
{
    size_t o = 0;
    for (; *in && o + 4 < size; in++) {
        unsigned char c = *in;
        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            out[o++] = c;
        } else {
            o += snprintf(out + o, size - o, "%%%02X", c);
        }
    }
    out[o] = '\0';
}

// City -> latitude/longitude via Open-Meteo geocoding.
// Searches in Portuguese, English and Spanish and keeps the most populous result; otherwise
// "Lisboa" lands in Mozambique and "Munich" doesn't match "Munique".
static bool geocode(const char *city, double *lat_out, double *lon_out)
{
    char name[3 * sizeof(g_cfg.city)];
    net_url_encode(city, name, sizeof(name));

    static const char *LANGS[] = {"pt", "en", "es"};
    double best_pop = -1, lat = 0, lon = 0;
    for (int l = 0; l < 3; l++) {
        char url[256];
        snprintf(url, sizeof(url),
                 "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=10&language=%s&format=json",
                 name, LANGS[l]);
        cJSON *root = net_http_json(url, NULL, 10000);
        const cJSON *r;
        cJSON_ArrayForEach(r, cJSON_GetObjectItem(root, "results")) {
            double pop = json_num(r, "population");
            if (pop > best_pop) {
                best_pop = pop;
                lat = json_num(r, "latitude");
                lon = json_num(r, "longitude");
            }
        }
        cJSON_Delete(root);
    }
    if (best_pop < 0) {
        return false;
    }
    *lat_out = lat;
    *lon_out = lon;
    ESP_LOGI(TAG, "city '%s' -> %.4f, %.4f (pop %.0f)", city, lat, lon, best_pop);
    return true;
}

static bool geocode_city(void)
{
    double lat, lon;
    if (!geocode(g_cfg.city, &lat, &lon)) {
        return false;
    }
    snprintf(g_cfg.lat, sizeof(g_cfg.lat), "%.4f", lat);
    snprintf(g_cfg.lon, sizeof(g_cfg.lon), "%.4f", lon);
    config_save();
    return true;
}

// UTC offset (seconds) -> POSIX TZ, e.g. 7200 -> "<+02>-2", -10800 -> "<-03>3"
static void apply_utc_offset(int offset)
{
    int a = abs(offset);
    int h = a / 3600, m = (a % 3600) / 60;
    char sign = offset >= 0 ? '+' : '-';
    const char *posix_sign = offset >= 0 ? "-" : "";
    char tz[sizeof(g_cfg.tz)];
    if (m) {
        snprintf(tz, sizeof(tz), "<%c%02d%02d>%s%d:%02d", sign, h, m, posix_sign, h, m);
    } else {
        snprintf(tz, sizeof(tz), "<%c%02d>%s%d", sign, h, posix_sign, h);
    }
    if (strcmp(tz, g_cfg.tz) != 0) {
        ESP_LOGI(TAG, "time zone: %s -> %s", g_cfg.tz, tz);
        strlcpy(g_cfg.tz, tz, sizeof(g_cfg.tz));
        setenv("TZ", g_cfg.tz, 1);
        tzset();
        config_save();
    }
}

static bool fetch_weather(weather_t *out)
{
    char url[320];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset&timezone=auto&forecast_days=%d",
             g_cfg.lat, g_cfg.lon, FORECAST_DAYS);

    cJSON *root = net_http_json(url, NULL, 10000);
    const cJSON *cur = cJSON_GetObjectItem(root, "current");
    const cJSON *daily = cJSON_GetObjectItem(root, "daily");
    bool ok = cur && daily;
    if (ok) {
        apply_utc_offset((int)json_num(root, "utc_offset_seconds"));

        out->temp = json_num(cur, "temperature_2m");
        out->humidity = (int)json_num(cur, "relative_humidity_2m");
        out->code = (int)json_num(cur, "weather_code");
        out->wind_kmh = json_num(cur, "wind_speed_10m");

        const cJSON *dates = cJSON_GetObjectItem(daily, "time");
        out->n_days = cJSON_GetArraySize(dates);
        if (out->n_days > FORECAST_DAYS) {
            out->n_days = FORECAST_DAYS;
        }
        for (int i = 0; i < out->n_days; i++) {
            forecast_day_t *d = &out->days[i];
            d->wday = weekday_of(cJSON_GetStringValue(cJSON_GetArrayItem(dates, i)));
            d->code = (int)json_at(daily, "weather_code", i);
            d->temp_max = json_at(daily, "temperature_2m_max", i);
            d->temp_min = json_at(daily, "temperature_2m_min", i);
        }
        out->updated_at = time(NULL);
        out->utc_offset = (int)json_num(root, "utc_offset_seconds");
        ESP_LOGI(TAG, "weather: %.1f C, code %d, %d days", out->temp, out->code, out->n_days);

        // today's sunrise/sunset, in the city's local time ("2026-10-13T07:28")
        const char *rise = cJSON_GetStringValue(cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "sunrise"), 0));
        const char *set = cJSON_GetStringValue(cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "sunset"), 0));
        struct tm r = {.tm_isdst = -1}, s = {.tm_isdst = -1};
        if (rise && set &&
            sscanf(rise, "%d-%d-%dT%d:%d", &r.tm_year, &r.tm_mon, &r.tm_mday, &r.tm_hour, &r.tm_min) == 5 &&
            sscanf(set, "%d-%d-%dT%d:%d", &s.tm_year, &s.tm_mon, &s.tm_mday, &s.tm_hour, &s.tm_min) == 5) {
            r.tm_year -= 1900;
            r.tm_mon -= 1;
            s.tm_year -= 1900;
            s.tm_mon -= 1;
            dimmer_set_sun(mktime(&r), mktime(&s));
            ESP_LOGI(TAG, "sun: rises %02d:%02d, sets %02d:%02d", r.tm_hour, r.tm_min, s.tm_hour, s.tm_min);
        }
    }
    cJSON_Delete(root);
    return ok;
}

// ---------- other cities ----------

static city_wx_t s_cities[CITIES_MAX];
static double s_city_lat[CITIES_MAX], s_city_lon[CITIES_MAX];
static bool s_city_geo[CITIES_MAX];
static int s_ncities;

// "Florianópolis, Tokyo" -> names (trimmed, up to CITIES_MAX)
static void parse_cities(void)
{
    char list[sizeof(g_cfg.cities)];
    strlcpy(list, g_cfg.cities, sizeof(list));
    s_ncities = 0;
    for (char *save, *tok = strtok_r(list, ",;", &save); tok && s_ncities < CITIES_MAX;
         tok = strtok_r(NULL, ",;", &save)) {
        tok += strspn(tok, " ");
        for (char *e = tok + strlen(tok); e > tok && e[-1] == ' '; *--e = '\0') {
        }
        if (*tok) {
            strlcpy(s_cities[s_ncities].name, tok, sizeof(s_cities[s_ncities].name));
            s_ncities++;
        }
    }
}

static void fetch_city(int i)
{
    city_wx_t *c = &s_cities[i];
    if (!s_city_geo[i]) {
        s_city_geo[i] = geocode(c->name, &s_city_lat[i], &s_city_lon[i]);
        if (!s_city_geo[i]) {
            return;
        }
    }
    char url[300];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code,is_day&daily=temperature_2m_max,temperature_2m_min"
             "&timezone=auto&forecast_days=1",
             s_city_lat[i], s_city_lon[i]);
    cJSON *root = net_http_json(url, NULL, 10000);
    const cJSON *cur = cJSON_GetObjectItem(root, "current");
    const cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (cur && daily) {
        c->utc_offset = (int)json_num(root, "utc_offset_seconds");
        c->temp = json_num(cur, "temperature_2m");
        c->code = (int)json_num(cur, "weather_code");
        c->is_day = json_num(cur, "is_day") != 0;
        c->tmax = json_at(daily, "temperature_2m_max", 0);
        c->tmin = json_at(daily, "temperature_2m_min", 0);
        c->ok = true;
        ESP_LOGI(TAG, "%s: %.1f C, code %d, UTC%+d min", c->name, c->temp, c->code, c->utc_offset / 60);
    }
    cJSON_Delete(root);
}

static void net_task(void *arg)
{
    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT, false, true,
                                           pdMS_TO_TICKS(FIRST_CONNECT_MS));
    if (!(bits & CONNECTED_BIT)) {
        ESP_LOGW(TAG, "could not connect to '%s', opening the portal", g_cfg.ssid);
        char msg[64];
        snprintf(msg, sizeof(msg), LV_SYMBOL_WARNING " %s", T->no_wifi_portal);
        set_status(msg);
        vTaskDelay(pdMS_TO_TICKS(2000));
        config_request_portal();
    }

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK) {
        ESP_LOGW(TAG, "NTP not synced yet, still trying in the background");
    }

    while (!g_cfg.lat[0] || !g_cfg.lon[0]) {
        set_status(T->finding_city);
        if (geocode_city()) {
            break;
        }
        char status[96];
        int n = snprintf(status, sizeof(status), LV_SYMBOL_WARNING " ");
        snprintf(status + n, sizeof(status) - n, T->city_not_found_fmt, g_cfg.city);
        set_status(status);
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    }

    msg_init();
    if (g_cfg.tg_token[0]) {
        telegram_start();
    }
    if (g_cfg.govee_key[0]) {
        govee_start();
    }

    while (true) {
        xEventGroupWaitBits(s_events, CONNECTED_BIT, false, true, portMAX_DELAY);
        weather_t w;
        bool ok = fetch_weather(&w);
        if (ok && lvgl_port_lock(0)) {
            ui_set_weather(&w);
            lvgl_port_unlock();
            s_last_weather = w;
            s_has_weather = true;
            s_weather_color = ui_weather_color(w.code);
            led_solid(s_weather_color);
        }
        for (int i = 0; i < s_ncities; i++) {
            fetch_city(i);
        }
        if (s_ncities && lvgl_port_lock(0)) {
            ui_set_cities(s_cities, s_ncities);
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(ok ? WEATHER_PERIOD_MS : WEATHER_RETRY_MS));
    }
}

void net_start(void)
{
    setenv("TZ", g_cfg.tz, 1);
    tzset();

    parse_cities();
    s_events = xEventGroupCreate();
    wifi_init();
    xTaskCreate(net_task, "net", 8192, NULL, 4, NULL);
}

bool net_get_weather(weather_t *out)
{
    if (s_has_weather) {
        *out = s_last_weather;
    }
    return s_has_weather;
}
