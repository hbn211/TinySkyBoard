#include "config.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include "nvs.h"
#include "esp_system.h"
#include "sdkconfig.h"

// NVS namespace from the project's first name; kept so existing boards keep their settings
#define NS "painel"

config_t g_cfg;

static void get_str(nvs_handle_t nvs, const char *key, char *out, size_t size)
{
    size_t len = size;
    if (nvs_get_str(nvs, key, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
}

static bool has_key(nvs_handle_t nvs, const char *key)
{
    size_t len = 0;
    return nvs_get_str(nvs, key, NULL, &len) == ESP_OK;
}

// "Munich, Florianópolis" typed as the main city: the first one stays, the rest become other cities.
// Returns true if it changed something.
bool config_split_city(void)
{
    char *comma = strpbrk(g_cfg.city, ",;");
    if (!comma) {
        return false;
    }
    char rest[sizeof(g_cfg.city)];
    strlcpy(rest, comma + 1, sizeof(rest));
    *comma = '\0';
    for (char *e = g_cfg.city + strlen(g_cfg.city); e > g_cfg.city && e[-1] == ' '; *--e = '\0') {
    }
    char cities[sizeof(g_cfg.cities)];
    strlcpy(cities, rest + strspn(rest, " "), sizeof(cities));
    if (g_cfg.cities[0]) {
        strlcat(cities, ", ", sizeof(cities));
        strlcat(cities, g_cfg.cities, sizeof(cities));
    }
    strlcpy(g_cfg.cities, cities, sizeof(g_cfg.cities));
    g_cfg.lat[0] = g_cfg.lon[0] = '\0';   // look the main city up again
    return true;
}

void config_load(void)
{
    g_cfg.work_from = WORK_DEFAULT_FROM;
    g_cfg.work_to = WORK_DEFAULT_TO;
    g_cfg.work_days = WORK_DEFAULT_DAYS;
    strlcpy(g_cfg.ssid, CONFIG_TINYSKY_WIFI_SSID, sizeof(g_cfg.ssid));
    strlcpy(g_cfg.pass, CONFIG_TINYSKY_WIFI_PASSWORD, sizeof(g_cfg.pass));
    strlcpy(g_cfg.city, CONFIG_TINYSKY_CITY, sizeof(g_cfg.city));
    strlcpy(g_cfg.lat, CONFIG_TINYSKY_LAT, sizeof(g_cfg.lat));
    strlcpy(g_cfg.lon, CONFIG_TINYSKY_LON, sizeof(g_cfg.lon));
    strlcpy(g_cfg.tz, CONFIG_TINYSKY_TZ, sizeof(g_cfg.tz));

    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    if (has_key(nvs, "ssid")) {
        get_str(nvs, "ssid", g_cfg.ssid, sizeof(g_cfg.ssid));
        get_str(nvs, "pass", g_cfg.pass, sizeof(g_cfg.pass));
    }
    if (has_key(nvs, "city")) {
        get_str(nvs, "city", g_cfg.city, sizeof(g_cfg.city));
        get_str(nvs, "lat", g_cfg.lat, sizeof(g_cfg.lat));
        get_str(nvs, "lon", g_cfg.lon, sizeof(g_cfg.lon));
    }
    if (has_key(nvs, "tz")) {
        get_str(nvs, "tz", g_cfg.tz, sizeof(g_cfg.tz));
    }
    get_str(nvs, "tg_token", g_cfg.tg_token, sizeof(g_cfg.tg_token));
    nvs_get_i64(nvs, "tg_owner", &g_cfg.tg_owner);
    get_str(nvs, "govee_key", g_cfg.govee_key, sizeof(g_cfg.govee_key));
    get_str(nvs, "cities", g_cfg.cities, sizeof(g_cfg.cities));
    uint8_t h12 = 0;
    nvs_get_u8(nvs, "h12", &h12);
    g_cfg.h12 = h12;
    get_str(nvs, "work_city", g_cfg.work_city, sizeof(g_cfg.work_city));
    // hours and minutes in separate keys (older versions saved only the hours)
    static const char *const KEYS[][2] = {{"work_from", "work_fm"}, {"work_to", "work_tm"}};
    uint16_t *dst[] = {&g_cfg.work_from, &g_cfg.work_to};
    for (int k = 0; k < 2; k++) {
        uint8_t h = *dst[k] / 60, m = 0;
        nvs_get_u8(nvs, KEYS[k][0], &h);
        nvs_get_u8(nvs, KEYS[k][1], &m);
        *dst[k] = (h % 24) * 60 + m % 60;
    }
    nvs_get_u8(nvs, "work_days", &g_cfg.work_days);
    nvs_close(nvs);
    if (config_split_city()) {
        config_save();
    }
}

void config_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_str(nvs, "ssid", g_cfg.ssid);
    nvs_set_str(nvs, "pass", g_cfg.pass);
    nvs_set_str(nvs, "city", g_cfg.city);
    nvs_set_str(nvs, "lat", g_cfg.lat);
    nvs_set_str(nvs, "lon", g_cfg.lon);
    nvs_set_str(nvs, "tz", g_cfg.tz);
    nvs_set_str(nvs, "tg_token", g_cfg.tg_token);
    nvs_set_i64(nvs, "tg_owner", g_cfg.tg_owner);
    nvs_set_str(nvs, "govee_key", g_cfg.govee_key);
    nvs_set_str(nvs, "cities", g_cfg.cities);
    nvs_set_str(nvs, "work_city", g_cfg.work_city);
    nvs_set_u8(nvs, "work_from", g_cfg.work_from / 60);
    nvs_set_u8(nvs, "work_fm", g_cfg.work_from % 60);
    nvs_set_u8(nvs, "work_to", g_cfg.work_to / 60);
    nvs_set_u8(nvs, "work_tm", g_cfg.work_to % 60);
    nvs_set_u8(nvs, "work_days", g_cfg.work_days);
    nvs_commit(nvs);
    nvs_close(nvs);
}

void clock_fmt(char *out, size_t size, int hour, int min)
{
    if (g_cfg.h12) {
        snprintf(out, size, "%d:%02d %s", hour % 12 ? hour % 12 : 12, min, hour < 12 ? "AM" : "PM");
    } else {
        snprintf(out, size, "%02d:%02d", hour, min);
    }
}

bool work_time(const struct tm *tm, int early_h)
{
    if (!g_cfg.work_city[0] || !(g_cfg.work_days & (1 << tm->tm_wday))) {
        return false;
    }
    int from = (g_cfg.work_from + 24 * 60 - early_h * 60) % (24 * 60);
    return in_window(tm->tm_hour * 60 + tm->tm_min, from, g_cfg.work_to);
}

bool in_window(int min, int from, int to)
{
    return from <= to ? min >= from && min < to : min >= from || min < to;
}

uint8_t config_get_u8(const char *key)
{
    uint8_t val = 0;
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, key, &val);
        nvs_close(nvs);
    }
    return val;
}

uint8_t config_get_u8_def(const char *key, uint8_t def)
{
    uint8_t val = def;
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, key, &val);
        nvs_close(nvs);
    }
    return val;
}

void config_get_str(const char *key, char *out, size_t size)
{
    out[0] = '\0';
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = size;
        if (nvs_get_str(nvs, key, out, &len) != ESP_OK) {
            out[0] = '\0';
        }
        nvs_close(nvs);
    }
}

void config_set_str(const char *key, const char *val)
{
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, key, val);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void config_set_u8(const char *key, uint8_t val)
{
    nvs_handle_t nvs;
    if (nvs_open(NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, key, val);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void config_request_portal(void)
{
    config_set_u8("portal", 1);
    esp_restart();
}

bool config_take_portal_request(void)
{
    if (!config_get_u8("portal")) {
        return false;
    }
    config_set_u8("portal", 0);
    return true;
}
