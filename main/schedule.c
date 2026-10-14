#include "schedule.h"
#include "app.h"

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"

#define CHECK_MS        5000
#define KEY_SCHED       0x80000000u   // scheduled item key = schedule id | KEY_SCHED

static const char *TAG = "schedule";

typedef struct {
    uint32_t id;            // 0 = free slot
    int64_t when;
    char text[MSG_TEXT];
} direct_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static sched_t s_sched[SCHED_MAX];
static direct_t s_direct[MSG_DIRECT_MAX];
static uint32_t s_next_direct_id = 1;

// per schedule slot: occurrence already announced and occurrence hidden by the user
static time_t s_announced[SCHED_MAX];
static time_t s_dismissed[SCHED_MAX];
static uint32_t s_active_mask;          // which slots were active at the last evaluation
static volatile uint32_t s_events;

static void save_sched(void)
{
    nvs_handle_t nvs;
    if (nvs_open("painel", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_blob(nvs, "sched", s_sched, sizeof(s_sched));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void save_direct(void)
{
    nvs_handle_t nvs;
    if (nvs_open("painel", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_blob(nvs, "msgs", s_direct, sizeof(s_direct));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

bool sched_is_active(const sched_t *s, time_t now, time_t *occ)
{
    if (!s->id) {
        return false;
    }
    if (s->repeat == REPEAT_ONCE) {
        *occ = (time_t)s->start;
        return now >= *occ && now < *occ + s->dur_min * 60;
    }
    // today's or yesterday's occurrence (if it crosses midnight)
    for (int back = 0; back <= 1; back++) {
        struct tm t;
        localtime_r(&now, &t);
        t.tm_mday -= back;
        t.tm_hour = s->hour;
        t.tm_min = s->minute;
        t.tm_sec = 0;
        t.tm_isdst = -1;
        time_t o = mktime(&t);
        bool weekend = t.tm_wday == 0 || t.tm_wday == 6;
        if ((s->repeat == REPEAT_WEEKDAYS && weekend) || (s->repeat == REPEAT_WEEKENDS && !weekend)) {
            continue;
        }
        if (now >= o && now < o + s->dur_min * 60) {
            *occ = o;
            return true;
        }
    }
    return false;
}

static bool clock_ok(time_t now)
{
    struct tm tm;
    localtime_r(&now, &tm);
    return tm.tm_year >= (2024 - 1900);
}

// active and not hidden by the user; call with s_lock held
static bool slot_visible(int i, time_t now, time_t *occ)
{
    return sched_is_active(&s_sched[i], now, occ) && *occ != s_dismissed[i];
}

static void evaluate(void)
{
    time_t now = time(NULL);
    if (!clock_ok(now)) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool expired = false;
    for (int i = 0; i < SCHED_MAX; i++) {
        sched_t *s = &s_sched[i];
        if (s->id && s->repeat == REPEAT_ONCE && now >= s->start + s->dur_min * 60) {
            ESP_LOGI(TAG, "expired: %s", s->text);
            memset(s, 0, sizeof(*s));
            expired = true;
        }
    }
    if (expired) {
        save_sched();
    }

    uint32_t mask = 0;
    for (int i = 0; i < SCHED_MAX; i++) {
        time_t occ;
        if (slot_visible(i, now, &occ)) {
            mask |= 1u << i;
            if (occ != s_announced[i]) {
                ESP_LOGI(TAG, "started: %s", s_sched[i].text);
                s_announced[i] = occ;
                s_events |= MSG_EV_NEW | MSG_EV_CHANGED;
            }
        }
    }
    if (mask != s_active_mask) {
        s_events |= MSG_EV_CHANGED;
        s_active_mask = mask;
    }
    xSemaphoreGive(s_lock);
}

static void msg_task(void *arg)
{
    while (true) {
        evaluate();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CHECK_MS));
    }
}

void msg_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    nvs_handle_t nvs;
    if (nvs_open("painel", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_sched);
        if (nvs_get_blob(nvs, "sched", s_sched, &len) != ESP_OK || len != sizeof(s_sched)) {
            memset(s_sched, 0, sizeof(s_sched));   // nothing saved or old format
        }
        len = sizeof(s_direct);
        if (nvs_get_blob(nvs, "msgs", s_direct, &len) != ESP_OK || len != sizeof(s_direct)) {
            memset(s_direct, 0, sizeof(s_direct));
            // the previous version stored a single message
            len = sizeof(s_direct[0].text);
            if (nvs_get_str(nvs, "msg", s_direct[0].text, &len) == ESP_OK && s_direct[0].text[0]) {
                s_direct[0].id = 1;
                nvs_get_i64(nvs, "msg_t", &s_direct[0].when);
            }
        }
        nvs_close(nvs);
    }
    for (int i = 0; i < MSG_DIRECT_MAX; i++) {
        if (s_direct[i].id >= s_next_direct_id) {
            s_next_direct_id = s_direct[i].id + 1;
        }
    }
    // schedules already live before boot don't count as "new"
    time_t now = time(NULL);
    for (int i = 0; i < SCHED_MAX; i++) {
        time_t occ;
        if (clock_ok(now) && sched_is_active(&s_sched[i], now, &occ)) {
            s_announced[i] = occ;
        }
    }
    s_events = MSG_EV_CHANGED;
    xTaskCreate(msg_task, "agenda", 4096, NULL, 3, &s_task);
}

int msg_add_direct(const char *text)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int slot = 0;
    for (int i = 0; i < MSG_DIRECT_MAX; i++) {
        if (!s_direct[i].id) {
            slot = i;
            break;
        }
        if (s_direct[i].when < s_direct[slot].when) {
            slot = i;   // full: replace the oldest
        }
    }
    s_direct[slot].id = s_next_direct_id++;
    s_direct[slot].when = time(NULL);
    strlcpy(s_direct[slot].text, text, sizeof(s_direct[slot].text));
    save_direct();
    int n = 0;
    for (int i = 0; i < MSG_DIRECT_MAX; i++) {
        n += s_direct[i].id != 0;
    }
    s_events |= MSG_EV_NEW | MSG_EV_CHANGED;
    xSemaphoreGive(s_lock);
    return n;
}

int msg_active(msg_item_t *out, int max)
{
    time_t now = time(NULL);
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MSG_DIRECT_MAX && n < max; i++) {
        if (s_direct[i].id) {
            out[n].key = s_direct[i].id;
            out[n].scheduled = false;
            out[n].when = (time_t)s_direct[i].when;
            strlcpy(out[n].text, s_direct[i].text, sizeof(out[n].text));
            n++;
        }
    }
    for (int i = 0; i < SCHED_MAX && n < max; i++) {
        time_t occ;
        if (clock_ok(now) && slot_visible(i, now, &occ)) {
            out[n].key = s_sched[i].id | KEY_SCHED;
            out[n].scheduled = true;
            out[n].when = occ;
            strlcpy(out[n].text, s_sched[i].text, sizeof(out[n].text));
            n++;
        }
    }
    xSemaphoreGive(s_lock);

    // newest first (small list: simple insertion sort)
    for (int i = 1; i < n; i++) {
        msg_item_t tmp = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].when < tmp.when) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = tmp;
    }
    return n;
}

bool msg_delete(uint32_t key)
{
    bool ok = false;
    time_t now = time(NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (key & KEY_SCHED) {
        uint32_t id = key & ~KEY_SCHED;
        for (int i = 0; i < SCHED_MAX && !ok; i++) {
            time_t occ;
            if (s_sched[i].id == id && sched_is_active(&s_sched[i], now, &occ)) {
                if (s_sched[i].repeat == REPEAT_ONCE) {
                    ESP_LOGI(TAG, "deleted with the button: %s", s_sched[i].text);
                    memset(&s_sched[i], 0, sizeof(s_sched[i]));
                    save_sched();
                } else {
                    s_dismissed[i] = occ;   // comes back on the next repeat
                }
                ok = true;
            }
        }
    } else {
        for (int i = 0; i < MSG_DIRECT_MAX && !ok; i++) {
            if (s_direct[i].id == key) {
                memset(&s_direct[i], 0, sizeof(s_direct[i]));
                save_direct();
                ok = true;
            }
        }
    }
    if (ok) {
        s_events |= MSG_EV_CHANGED;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void msg_clear_all(void)
{
    time_t now = time(NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_direct, 0, sizeof(s_direct));
    save_direct();
    for (int i = 0; i < SCHED_MAX; i++) {
        time_t occ;
        if (sched_is_active(&s_sched[i], now, &occ)) {
            s_dismissed[i] = occ;
        }
    }
    s_events |= MSG_EV_CHANGED;
    xSemaphoreGive(s_lock);
}

uint32_t msg_take_events(void)
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t ev = s_events;
    s_events = 0;
    xSemaphoreGive(s_lock);
    return ev;
}

int sched_list(sched_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    for (int i = 0; i < SCHED_MAX; i++) {
        if (s_sched[i].id) {
            out[n++] = s_sched[i];
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

bool sched_add(const sched_t *s)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t next_id = 1;
    for (int i = 0; i < SCHED_MAX; i++) {
        if (s_sched[i].id >= next_id) {
            next_id = s_sched[i].id + 1;
        }
    }
    for (int i = 0; i < SCHED_MAX && !ok; i++) {
        if (!s_sched[i].id) {
            s_sched[i] = *s;
            s_sched[i].id = next_id;
            s_announced[i] = 0;
            s_dismissed[i] = 0;
            ok = true;
        }
    }
    if (ok) {
        save_sched();
        ESP_LOGI(TAG, "new schedule %" PRIu32 ": %s", next_id, s->text);
    }
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);   // "Now" shows up without waiting for the next cycle
    return ok;
}

bool sched_remove(int index)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0, n = 0; i < SCHED_MAX && !ok; i++) {
        if (s_sched[i].id && n++ == index) {
            memset(&s_sched[i], 0, sizeof(s_sched[i]));
            ok = true;
        }
    }
    if (ok) {
        save_sched();
    }
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return ok;
}
