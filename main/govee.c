// Govee thermometers through the cloud OpenAPI 2.0 (https://developer.govee.com)
#include "app.h"
#include "config.h"
#include "i18n.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_timer.h"
#include "cJSON.h"

#define API             "https://openapi.api.govee.com/router/api/v1"
#define KEY_HEADER      "Govee-API-Key"
#define LIST_EVERY_S    3600 // re-read the device list every hour: new or renamed sensors
#define WAKE_EARLY_S    30   // start polling this long before the next expected upload
#define SEARCH_EXTRA_S  60   // after a full interval + this without a change, poll slowly
#define SLOW_S          60   // polling of a sensor that missed its upload
#define HIST_N          16   // readings kept for the trend
#define TREND_WINDOW_S  (60 * 60)
#define TREND_MIN_S     (30 * 60)   // trend only with at least 30 min of history
#define TREND_C         0.3f // change over the window that counts as rising/falling

static const char *TAG = "govee";

typedef struct {
    char sku[16];
    char device[32];
    bool has_value;
    bool online;
    int64_t next_poll;      // monotonic seconds
    int64_t search_start;   // polling fast since then; 0 = waiting for the next upload
    int64_t alive_at;       // last change (or the first reading), monotonic
    float hist[HIST_N];
    time_t hist_t[HIST_N];
    int hist_n;
    int day;                // tm_yday + 1 of day_min/day_max
    uint8_t alerted;        // ALERT_* already sent (cleared when back in range)
} govee_track_t;

#define ALERT_T_LOW   0x01
#define ALERT_T_HIGH  0x02
#define ALERT_H_LOW   0x04
#define ALERT_H_HIGH  0x08
#define ALERT_SILENT  0x10
#define HYST_T        5     // tenths of a degree back inside the range before "back to normal"
#define HYST_H        2     // percent

static govee_sensor_t s_sensors[GOVEE_MAX];
static govee_track_t s_track[GOVEE_MAX];
static int s_n;
static int s_interval_s;
static SemaphoreHandle_t s_lock;
static volatile uint32_t s_version;
static volatile int s_http;   // last API answer, for the status card
static govee_alert_t s_alerts[GOVEE_MAX];
static int s_nalerts;
static bool s_alert_silent;   // "a sensor stopped sending" alert (setup page)

static int64_t mono_s(void)
{
    return esp_timer_get_time() / 1000000;
}

static bool fetch_list(void)
{
    int http;
    cJSON *root = net_http_json_hdr(API "/user/devices", NULL, KEY_HEADER, g_cfg.govee_key, 10000, &http);
    s_http = http;
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsArray(data)) {
        cJSON_Delete(root);
        return false;
    }
    // static: ~3 KB, too much for the stack next to TLS (only this task calls it)
    static govee_sensor_t sensors[GOVEE_MAX];
    static govee_track_t track[GOVEE_MAX];
    memset(sensors, 0, sizeof(sensors));
    memset(track, 0, sizeof(track));
    int n = 0;
    cJSON *d;
    cJSON_ArrayForEach(d, data) {
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(d, "type"));
        const char *sku = cJSON_GetStringValue(cJSON_GetObjectItem(d, "sku"));
        const char *dev = cJSON_GetStringValue(cJSON_GetObjectItem(d, "device"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(d, "deviceName"));
        if (n >= GOVEE_MAX || !type || !sku || !dev || strcmp(type, "devices.types.thermometer") != 0) {
            continue;
        }
        strlcpy(track[n].sku, sku, sizeof(track[n].sku));
        strlcpy(track[n].device, dev, sizeof(track[n].device));
        strlcpy(sensors[n].name, name ? name : sku, sizeof(sensors[n].name));
        // sensors we already knew keep their readings and their schedule
        for (int j = 0; j < s_n; j++) {
            if (strcmp(dev, s_track[j].device) == 0) {
                track[n] = s_track[j];
                char keep[sizeof(sensors[n].name)];
                strlcpy(keep, sensors[n].name, sizeof(keep));
                sensors[n] = s_sensors[j];
                strlcpy(sensors[n].name, keep, sizeof(sensors[n].name));
            }
        }
        n++;
    }
    cJSON_Delete(root);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_sensors, sensors, sizeof(sensors));
    memcpy(s_track, track, sizeof(track));
    s_n = n;
    s_version++;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%d thermometer(s)", n);
    return true;
}

// Day's min/max and the trend: the new reading against the oldest one of the last hour
static void add_reading(govee_sensor_t *s, govee_track_t *t, float temp_c, time_t now)
{
    struct tm tm;
    localtime_r(&now, &tm);
    if (t->day != tm.tm_yday + 1) {   // new day (or first reading): start the range again
        t->day = tm.tm_yday + 1;
        s->day_min = s->day_max = temp_c;
    }
    if (temp_c < s->day_min) {
        s->day_min = temp_c;
    }
    if (temp_c > s->day_max) {
        s->day_max = temp_c;
    }

    while (t->hist_n > 0 && (now - t->hist_t[0] > TREND_WINDOW_S || t->hist_n == HIST_N)) {
        memmove(t->hist, t->hist + 1, (t->hist_n - 1) * sizeof(t->hist[0]));
        memmove(t->hist_t, t->hist_t + 1, (t->hist_n - 1) * sizeof(t->hist_t[0]));
        t->hist_n--;
    }
    t->hist[t->hist_n] = temp_c;
    t->hist_t[t->hist_n] = now;
    t->hist_n++;

    s->has_trend = now - t->hist_t[0] >= TREND_MIN_S;
    float delta = temp_c - t->hist[0];
    s->trend = !s->has_trend ? 0 : delta >= TREND_C ? 1 : delta <= -TREND_C ? -1 : 0;
}

// ---------- alerts ----------

int govee_alerts_load(govee_alert_t *out, int max)
{
    nvs_handle_t nvs;
    int n = 0;
    if (nvs_open("painel", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = max * sizeof(*out);
        if (nvs_get_blob(nvs, "galert", out, &len) == ESP_OK) {
            n = len / sizeof(*out);
        }
        nvs_close(nvs);
    }
    return n;
}

void govee_alerts_save(const govee_alert_t *in, int n)
{
    nvs_handle_t nvs;
    if (nvs_open("painel", NVS_READWRITE, &nvs) == ESP_OK) {
        if (n > 0) {
            nvs_set_blob(nvs, "galert", in, n * sizeof(*in));
        } else {
            nvs_erase_key(nvs, "galert");
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

int govee_cmp_load(govee_cmp_t *out)
{
    memset(out, 0, GOVEE_CMP_MAX * sizeof(*out));
    nvs_handle_t nvs;
    if (nvs_open("painel", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = GOVEE_CMP_MAX * sizeof(*out);
        if (nvs_get_blob(nvs, "cmp", out, &len) != ESP_OK || len != GOVEE_CMP_MAX * sizeof(*out)) {
            memset(out, 0, GOVEE_CMP_MAX * sizeof(*out));
        }
        nvs_close(nvs);
    }
    int n = 0;
    for (int i = 0; i < GOVEE_CMP_MAX; i++) {
        n += out[i].a[0] && out[i].b[0];
    }
    return n;
}

void govee_cmp_save(const govee_cmp_t *in)
{
    nvs_handle_t nvs;
    if (nvs_open("painel", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_blob(nvs, "cmp", in, GOVEE_CMP_MAX * sizeof(*in));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

int govee_known(govee_ident_t *out, int max)
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_n < max ? s_n : max;
    for (int i = 0; i < n; i++) {
        strlcpy(out[i].device, s_track[i].device, sizeof(out[i].device));
        strlcpy(out[i].name, s_sensors[i].name, sizeof(out[i].name));
    }
    xSemaphoreGive(s_lock);
    return n;
}

static const govee_alert_t *alert_for(const char *device)
{
    for (int i = 0; i < s_nalerts; i++) {
        if (strcmp(s_alerts[i].device, device) == 0) {
            return &s_alerts[i];
        }
    }
    return NULL;
}

// temperature in the panel's unit, for the messages
static void fmt_temp(char *out, size_t size, float c)
{
    bool f = config_get_u8("fahr");
    snprintf(out, size, "%.1f\xC2\xB0%c", f ? c * 9 / 5 + 32 : c, f ? 'F' : 'C');
}

// One reading against the sensor's limits: one message when it leaves the range, one when it's back
static void check_alerts(int i, float temp_c, float hum)
{
    govee_track_t *t = &s_track[i];
    const govee_alert_t *a = alert_for(t->device);
    if (!a) {
        return;
    }
    const char *name = s_sensors[i].name;
    int tt = (int)(temp_c * 10 + (temp_c >= 0 ? 0.5f : -0.5f));
    char msg[200], v[16], lim[16];
    uint8_t before = t->alerted & ~ALERT_SILENT;

    if (a->tmax != GOVEE_T_NONE) {
        if (!(t->alerted & ALERT_T_HIGH) && tt > a->tmax) {
            t->alerted |= ALERT_T_HIGH;
            fmt_temp(v, sizeof(v), temp_c);
            fmt_temp(lim, sizeof(lim), a->tmax / 10.0f);
            snprintf(msg, sizeof(msg), T->al_t_high_fmt, name, v, lim);
            telegram_notify(msg);
        } else if ((t->alerted & ALERT_T_HIGH) && tt <= a->tmax - HYST_T) {
            t->alerted &= ~ALERT_T_HIGH;
        }
    }
    if (a->tmin != GOVEE_T_NONE) {
        if (!(t->alerted & ALERT_T_LOW) && tt < a->tmin) {
            t->alerted |= ALERT_T_LOW;
            fmt_temp(v, sizeof(v), temp_c);
            fmt_temp(lim, sizeof(lim), a->tmin / 10.0f);
            snprintf(msg, sizeof(msg), T->al_t_low_fmt, name, v, lim);
            telegram_notify(msg);
        } else if ((t->alerted & ALERT_T_LOW) && tt >= a->tmin + HYST_T) {
            t->alerted &= ~ALERT_T_LOW;
        }
    }
    if (hum >= 0 && a->hmax != GOVEE_H_NONE) {
        if (!(t->alerted & ALERT_H_HIGH) && hum > a->hmax) {
            t->alerted |= ALERT_H_HIGH;
            snprintf(msg, sizeof(msg), T->al_h_high_fmt, name, hum, a->hmax);
            telegram_notify(msg);
        } else if ((t->alerted & ALERT_H_HIGH) && hum <= a->hmax - HYST_H) {
            t->alerted &= ~ALERT_H_HIGH;
        }
    }
    if (hum >= 0 && a->hmin != GOVEE_H_NONE) {
        if (!(t->alerted & ALERT_H_LOW) && hum < a->hmin) {
            t->alerted |= ALERT_H_LOW;
            snprintf(msg, sizeof(msg), T->al_h_low_fmt, name, hum, a->hmin);
            telegram_notify(msg);
        } else if ((t->alerted & ALERT_H_LOW) && hum >= a->hmin + HYST_H) {
            t->alerted &= ~ALERT_H_LOW;
        }
    }
    if (before && !(t->alerted & ~ALERT_SILENT)) {
        fmt_temp(v, sizeof(v), temp_c);
        snprintf(msg, sizeof(msg), T->al_back_fmt, name, v, hum < 0 ? 0 : hum);
        telegram_notify(msg);
    }
}

static void poll_sensor(int i)
{
    static uint32_t req_id;
    govee_track_t *t = &s_track[i];
    char body[160];
    snprintf(body, sizeof(body), "{\"requestId\":\"tsb-%" PRIu32 "\",\"payload\":{\"sku\":\"%s\",\"device\":\"%s\"}}",
             ++req_id, t->sku, t->device);
    int http;
    cJSON *root = net_http_json_hdr(API "/device/state", body, KEY_HEADER, g_cfg.govee_key, 10000, &http);
    s_http = http;
    cJSON *caps = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "payload"), "capabilities");
    int64_t mono = mono_s();
    if (!cJSON_IsArray(caps)) {
        cJSON_Delete(root);
        t->next_poll = mono + SLOW_S;
        return;
    }
    bool online = true, has_t = false, has_h = false;
    float temp_f = 0, hum = -1;
    cJSON *c;
    cJSON_ArrayForEach(c, caps) {
        const char *inst = cJSON_GetStringValue(cJSON_GetObjectItem(c, "instance"));
        cJSON *v = cJSON_GetObjectItem(cJSON_GetObjectItem(c, "state"), "value");
        if (!inst || !v) {
            continue;
        }
        if (strcmp(inst, "online") == 0) {
            online = cJSON_IsTrue(v);
        } else if (strcmp(inst, "sensorTemperature") == 0 && cJSON_IsNumber(v)) {
            temp_f = (float)v->valuedouble;   // always Fahrenheit, whatever the app is set to
            has_t = true;
        } else if (strcmp(inst, "sensorHumidity") == 0 && cJSON_IsNumber(v)) {
            hum = (float)v->valuedouble;
            has_h = true;
        }
    }
    cJSON_Delete(root);
    if (!has_t) {
        t->next_poll = mono + SLOW_S;
        return;
    }

    time_t now = time(NULL);
    float temp_c = (temp_f - 32.0f) * 5.0f / 9.0f;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    govee_sensor_t *s = &s_sensors[i];
    bool changed = t->has_value && (temp_c != s->temp_c || (has_h && hum != s->humidity));
    t->online = online;
    if (!t->has_value || changed) {
        if (changed) {
            s->changed_at = now;   // new upload: the closest guess of when the sensor measured it
        }
        s->temp_c = temp_c;
        s->humidity = hum;
        t->has_value = true;
        t->alive_at = mono;
        add_reading(s, t, temp_c, now);
        s_version++;
    }
    xSemaphoreGive(s_lock);
    if (changed) {
        check_alerts(i, temp_c, hum);   // only new readings: the first one may be hours old
    }

    if (changed) {
        // in sync: sleep until just before the next upload
        t->search_start = 0;
        t->next_poll = mono + s_interval_s - WAKE_EARLY_S;
        ESP_LOGI(TAG, "%s: %.1f C %.1f%%, next look in %d s", s->name, temp_c, hum, s_interval_s - WAKE_EARLY_S);
    } else {
        if (!t->search_start) {
            t->search_start = mono;   // woke up for the next upload, or the first reading
        }
        // no upload within a whole interval: unchanged reading or a dead sensor, poll slowly
        bool missed = mono - t->search_start > s_interval_s + SEARCH_EXTRA_S;
        t->next_poll = mono + (missed ? SLOW_S : GOVEE_FAST_S);
    }
}

static bool visible(int i, int64_t mono)
{
    // only after a change was seen: until then the value's age is unknown (could be hours, if the sensor died)
    const govee_track_t *t = &s_track[i];
    return t->has_value && s_sensors[i].changed_at && t->online &&
           mono - t->alive_at <= (int64_t)GOVEE_STALE_UPLOADS * s_interval_s;
}

static void govee_task(void *arg)
{
    int64_t list_at = 0;
    bool was_visible[GOVEE_MAX] = {0};
    while (true) {
        int64_t mono = mono_s();
        if (!list_at || mono - list_at >= LIST_EVERY_S) {
            if (!fetch_list()) {
                vTaskDelay(pdMS_TO_TICKS(60 * 1000));   // no network or wrong key
                continue;
            }
            list_at = mono;
        }
        for (int i = 0; i < s_n; i++) {
            if (mono_s() >= s_track[i].next_poll) {
                poll_sensor(i);
            }
            // a sensor that goes quiet must leave the screen even without a new reading
            bool v = visible(i, mono_s());
            if (v != was_visible[i]) {
                was_visible[i] = v;
                s_version++;
                ESP_LOGI(TAG, "%s %s the screen", s_sensors[i].name, v ? "is on" : "left");
                govee_track_t *t = &s_track[i];
                char msg[200];
                if (!v && s_alert_silent && s_sensors[i].changed_at && !(t->alerted & ALERT_SILENT)) {
                    t->alerted |= ALERT_SILENT;
                    snprintf(msg, sizeof(msg), T->al_silent_fmt, s_sensors[i].name,
                             (int)((mono_s() - t->alive_at) / 60));
                    telegram_notify(msg);
                } else if (v && (t->alerted & ALERT_SILENT)) {
                    t->alerted &= ~ALERT_SILENT;
                    snprintf(msg, sizeof(msg), T->al_alive_fmt, s_sensors[i].name);
                    telegram_notify(msg);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void govee_start(void)
{
    s_interval_s = config_get_u8_def("govee_int", GOVEE_DEFAULT_MIN) * 60;
    s_nalerts = govee_alerts_load(s_alerts, GOVEE_MAX);
    s_alert_silent = config_get_u8("galert_q");
    s_lock = xSemaphoreCreateMutex();
    xTaskCreate(govee_task, "govee", 8192, NULL, 3, NULL);
}

int govee_get(govee_sensor_t *out, int max, bool all)
{
    if (!s_lock) {
        return 0;
    }
    int64_t mono = mono_s();
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_n && n < max; i++) {
        bool shown = visible(i, mono);
        if (s_track[i].has_value && (shown || all)) {
            out[n] = s_sensors[i];
            strlcpy(out[n].device, s_track[i].device, sizeof(out[n].device));
            out[n++].shown = shown;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

govee_status_t govee_status(void)
{
    govee_status_t st = {.on = s_lock != NULL, .http = s_http};
    if (!s_lock) {
        return st;
    }
    int64_t mono = mono_s();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    st.sensors = s_n;
    for (int i = 0; i < s_n; i++) {
        st.shown += visible(i, mono);
    }
    xSemaphoreGive(s_lock);
    return st;
}

uint32_t govee_version(void)
{
    return s_version;
}
