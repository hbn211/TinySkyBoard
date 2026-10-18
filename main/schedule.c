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
// unread = not opened with the button yet; the LED blinks while any is unread.
// Unread flags survive reboots: direct messages in "unread", schedules (for the current occurrence) in "sched_st".
static uint8_t s_unread_direct[MSG_DIRECT_MAX];
static bool s_unread_sched[SCHED_MAX];
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

static void save_unread(void)
{
    nvs_handle_t nvs;
    if (nvs_open("painel", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_blob(nvs, "unread", s_unread_direct, sizeof(s_unread_direct));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

// call with s_lock held, after any change to the unread flags
// Per-schedule occurrence state, saved so a reboot doesn't bring back a deleted occurrence
// or turn an unread one into read. The id ties it to the schedule that owned the slot.
typedef struct {
    uint32_t id;
    uint8_t unread;
    int64_t announced;
    int64_t dismissed;
} sched_state_t;

static void save_sched_state(void)
{
    sched_state_t st[SCHED_MAX] = {0};
    for (int i = 0; i < SCHED_MAX; i++) {
        st[i] = (sched_state_t){s_sched[i].id, s_unread_sched[i], s_announced[i], s_dismissed[i]};
    }
    nvs_handle_t nvs;
    if (nvs_open("painel", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_blob(nvs, "sched_st", st, sizeof(st));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

// true for the slots whose state was saved (same schedule id)
static uint32_t load_sched_state(nvs_handle_t nvs)
{
    sched_state_t st[SCHED_MAX];
    size_t len = sizeof(st);
    if (nvs_get_blob(nvs, "sched_st", st, &len) != ESP_OK || len != sizeof(st)) {
        return 0;
    }
    uint32_t loaded = 0;
    for (int i = 0; i < SCHED_MAX; i++) {
        if (s_sched[i].id && st[i].id == s_sched[i].id) {
            s_unread_sched[i] = st[i].unread;
            s_announced[i] = st[i].announced;
            s_dismissed[i] = st[i].dismissed;
            loaded |= 1u << i;
        }
    }
    return loaded;
}

static void update_led(void)
{
    bool any = false;
    for (int i = 0; i < MSG_DIRECT_MAX; i++) {
        any |= s_direct[i].id && s_unread_direct[i];
    }
    for (int i = 0; i < SCHED_MAX; i++) {
        any |= s_unread_sched[i];
    }
    led_set_alert(any);
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
    bool dirty = expired;
    for (int i = 0; i < SCHED_MAX; i++) {
        time_t occ;
        if (slot_visible(i, now, &occ)) {
            mask |= 1u << i;
            if (occ != s_announced[i]) {
                ESP_LOGI(TAG, "started: %s", s_sched[i].text);
                s_announced[i] = occ;
                s_unread_sched[i] = true;
                s_events |= MSG_EV_NEW | MSG_EV_CHANGED;
                dirty = true;
            }
        } else if (s_unread_sched[i]) {
            s_unread_sched[i] = false;   // ended, dismissed or removed
            dirty = true;
        }
    }
    if (dirty) {
        save_sched_state();
    }
    update_led();
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

    uint32_t loaded = 0;
    nvs_handle_t nvs;
    if (nvs_open("painel", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_sched);
        if (nvs_get_blob(nvs, "sched", s_sched, &len) != ESP_OK || len != sizeof(s_sched)) {
            memset(s_sched, 0, sizeof(s_sched));   // nothing saved or old format
        }
        len = sizeof(s_unread_direct);
        if (nvs_get_blob(nvs, "unread", s_unread_direct, &len) != ESP_OK || len != sizeof(s_unread_direct)) {
            memset(s_unread_direct, 0, sizeof(s_unread_direct));
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
        loaded = load_sched_state(nvs);
        nvs_close(nvs);
    }
    for (int i = 0; i < MSG_DIRECT_MAX; i++) {
        if (s_direct[i].id >= s_next_direct_id) {
            s_next_direct_id = s_direct[i].id + 1;
        }
    }
    // without saved state (first boot of this version), schedules already live don't count as "new"
    time_t now = time(NULL);
    for (int i = 0; i < SCHED_MAX; i++) {
        time_t occ;
        if (!(loaded & (1u << i)) && clock_ok(now) && sched_is_active(&s_sched[i], now, &occ)) {
            s_announced[i] = occ;
        }
    }
    update_led();
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
    s_unread_direct[slot] = 1;
    save_direct();
    save_unread();
    update_led();
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
            out[n].unread = s_unread_direct[i];
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
            out[n].unread = s_unread_sched[i];
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
                s_unread_sched[i] = false;
                save_sched_state();
                ok = true;
            }
        }
    } else {
        for (int i = 0; i < MSG_DIRECT_MAX && !ok; i++) {
            if (s_direct[i].id == key) {
                memset(&s_direct[i], 0, sizeof(s_direct[i]));
                s_unread_direct[i] = 0;
                save_direct();
                save_unread();
                ok = true;
            }
        }
    }
    if (ok) {
        s_events |= MSG_EV_CHANGED;
        update_led();
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool msg_mark_read(uint32_t key)
{
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (key & KEY_SCHED) {
        for (int i = 0; i < SCHED_MAX; i++) {
            if (s_sched[i].id == (key & ~KEY_SCHED) && s_unread_sched[i]) {
                s_unread_sched[i] = false;
                save_sched_state();
                changed = true;
            }
        }
    } else {
        for (int i = 0; i < MSG_DIRECT_MAX; i++) {
            if (s_direct[i].id == key && s_unread_direct[i]) {
                s_unread_direct[i] = 0;
                save_unread();
                changed = true;
            }
        }
    }
    if (changed) {
        update_led();
    }
    xSemaphoreGive(s_lock);
    return changed;
}

int msg_unread_count(void)
{
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MSG_DIRECT_MAX; i++) {
        n += s_direct[i].id && s_unread_direct[i];
    }
    for (int i = 0; i < SCHED_MAX; i++) {
        n += s_unread_sched[i];
    }
    xSemaphoreGive(s_lock);
    return n;
}

void msg_clear_all(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_direct, 0, sizeof(s_direct));
    memset(s_unread_direct, 0, sizeof(s_unread_direct));
    save_direct();
    save_unread();
    update_led();
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

int sched_state(uint32_t id)
{
    int st = SCHED_OFF;
    time_t now = time(NULL), occ;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SCHED_MAX; i++) {
        if (s_sched[i].id != id || !clock_ok(now)) {
            continue;
        }
        if (sched_is_active(&s_sched[i], now, &occ)) {
            st = occ == s_dismissed[i] ? SCHED_DISMISSED : SCHED_ON_SCREEN;
        } else if (s_dismissed[i] > now) {
            st = SCHED_SKIPPED;   // the next occurrence was skipped beforehand
        }
    }
    xSemaphoreGive(s_lock);
    return st;
}

// Start of the occurrence that is on now, or else the next one (repeating schedules); 0 if none in a week
static time_t current_or_next(const sched_t *s, time_t now)
{
    time_t occ;
    if (sched_is_active(s, now, &occ)) {
        return occ;
    }
    for (int d = 0; d <= 7; d++) {
        struct tm t;
        localtime_r(&now, &t);
        t.tm_mday += d;
        t.tm_hour = s->hour;
        t.tm_min = s->minute;
        t.tm_sec = 0;
        t.tm_isdst = -1;
        time_t o = mktime(&t);
        bool weekend = t.tm_wday == 0 || t.tm_wday == 6;
        if ((s->repeat == REPEAT_WEEKDAYS && weekend) || (s->repeat == REPEAT_WEEKENDS && !weekend)) {
            continue;
        }
        if (o > now) {
            return o;
        }
    }
    return 0;
}

bool sched_skip_today(uint32_t id)
{
    bool ok = false, once = false;
    time_t now = time(NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SCHED_MAX && !ok; i++) {
        if (s_sched[i].id != id) {
            continue;
        }
        if (s_sched[i].repeat == REPEAT_ONCE) {
            once = true;   // only one occurrence: same as deleting it
            break;
        }
        time_t occ = current_or_next(&s_sched[i], now);
        if (occ) {
            ESP_LOGI(TAG, "skipped one occurrence: %s", s_sched[i].text);
            s_dismissed[i] = occ;
            s_unread_sched[i] = false;
            save_sched_state();
            s_events |= MSG_EV_CHANGED;
            ok = true;
        }
    }
    if (ok) {
        update_led();
    }
    xSemaphoreGive(s_lock);
    if (once) {
        return sched_delete_id(id);
    }
    xTaskNotifyGive(s_task);
    return ok;
}

bool sched_delete_id(uint32_t id)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SCHED_MAX && !ok; i++) {
        if (s_sched[i].id == id) {
            ESP_LOGI(TAG, "deleted from the panel: %s", s_sched[i].text);
            memset(&s_sched[i], 0, sizeof(s_sched[i]));
            s_unread_sched[i] = false;
            save_sched();
            ok = true;
        }
    }
    if (ok) {
        update_led();
        s_events |= MSG_EV_CHANGED;
    }
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return ok;
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
            s_unread_sched[i] = false;
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
