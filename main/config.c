#include "config.h"

#include <string.h>
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

void config_load(void)
{
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
    nvs_close(nvs);
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
    nvs_commit(nvs);
    nvs_close(nvs);
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
