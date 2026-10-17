#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define SCHED_MAX       8
#define SCHED_TEXT      120
#define MSG_DIRECT_MAX  5       // direct messages taking turns; the oldest is dropped
#define MSG_TEXT        240
#define MSG_ITEMS_MAX   (MSG_DIRECT_MAX + SCHED_MAX)

typedef enum {
    REPEAT_ONCE = 0,
    REPEAT_DAILY,
    REPEAT_WEEKDAYS,
    REPEAT_WEEKENDS,
} repeat_t;

typedef struct {
    uint32_t id;            // 0 = free slot
    char text[SCHED_TEXT];
    int64_t start;          // REPEAT_ONCE: start (epoch)
    uint8_t hour, minute;   // repeats: local time of day
    uint8_t repeat;         // repeat_t
    uint16_t dur_min;
} sched_t;

// A message that is live right now: direct (Telegram) or a schedule within its window
typedef struct {
    uint32_t key;           // identifies the item for msg_delete()
    bool scheduled;
    bool unread;            // not opened with the button yet
    time_t when;            // arrival (direct) or start of this occurrence (scheduled)
    char text[MSG_TEXT];
} msg_item_t;

#define MSG_EV_CHANGED  (1 << 0)   // the active list changed
#define MSG_EV_NEW      (1 << 1)   // one arrived (or started): show it now

void msg_init(void);

int msg_add_direct(const char *text);    // returns how many direct messages are active
int msg_active(msg_item_t *out, int max);   // newest first
bool msg_delete(uint32_t key);           // repeating schedule: only hides the current occurrence
void msg_clear_all(void);                // /clear: removes the direct messages (schedules stay)
bool msg_mark_read(uint32_t key);        // opened with the button; false if it was already read
int msg_unread_count(void);
uint32_t msg_take_events(void);          // MSG_EV_*, since the last call

int sched_list(sched_t *out);            // copies the schedules in order; returns how many
bool sched_add(const sched_t *s);        // false if full
bool sched_remove(int index);            // list index (0 = first)
bool sched_is_active(const sched_t *s, time_t now, time_t *occ);
