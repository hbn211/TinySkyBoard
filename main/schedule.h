#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define SCHED_MAX       8
#define SCHED_TEXT      120
#define MSG_DIRECT_MAX  5       // direct messages taking turns; the oldest is dropped
#define MSG_TEXT        240
#define MSG_ITEMS_MAX   (MSG_DIRECT_MAX + SCHED_MAX)
#define MSG_FROM        24      // sender's first name (only shown when the bot has more than one person)

typedef enum {
    REPEAT_ONCE = 0,
    REPEAT_DAILY,
    REPEAT_WEEKDAYS,
    REPEAT_WEEKENDS,
} repeat_t;

typedef struct {
    uint32_t id;            // 0 = free slot
    char text[SCHED_TEXT];
    uint8_t who;            // who created it: 0 = the owner, n = bot member n (padding byte before)
    int64_t start;          // REPEAT_ONCE: start (epoch)
    uint8_t hour, minute;   // repeats: local time of day
    uint8_t repeat;         // repeat_t
    uint8_t flags;          // SCHED_F_* (a byte that was padding: saved schedules keep their size)
    uint16_t dur_min;
} sched_t;

#define SCHED_F_TG_DONE 0x01   // when it starts, also sent to Telegram with a "Done" button
#define SCHED_F_TG_ALL  0x02   // with SCHED_F_TG_DONE: to everyone on the bot, not only whoever scheduled it

// A message that is live right now: direct (Telegram) or a schedule within its window
typedef struct {
    uint32_t key;           // identifies the item for msg_delete()
    bool scheduled;
    bool unread;            // not opened with the button yet
    time_t when;            // arrival (direct) or start of this occurrence (scheduled)
    char from[MSG_FROM];    // sender's name; empty = don't show
    char text[MSG_TEXT];
} msg_item_t;

#define MSG_EV_CHANGED  (1 << 0)   // the active list changed
#define MSG_EV_NEW      (1 << 1)   // one arrived (or started): show it now

void msg_init(void);

int msg_add_direct(const char *text, const char *from);   // returns how many direct messages are active
int msg_active(msg_item_t *out, int max);   // newest first
bool msg_delete(uint32_t key);           // repeating schedule: only hides the current occurrence
void msg_clear_all(void);                // /clear: removes the direct messages (schedules stay)
bool msg_mark_read(uint32_t key);        // opened with the button; false if it was already read
int msg_unread_count(void);
uint32_t msg_take_events(void);          // MSG_EV_*, since the last call

int sched_list(sched_t *out);            // copies the schedules in order; returns how many
bool sched_add(const sched_t *s);        // false if full
bool sched_update(uint32_t id, const sched_t *s);   // replaces it keeping its id; starts over as new
bool sched_remove(int index);            // list index (0 = first)
bool sched_is_active(const sched_t *s, time_t now, time_t *occ);
enum { SCHED_OFF, SCHED_ON_SCREEN, SCHED_DISMISSED, SCHED_SKIPPED };
int sched_state(uint32_t id);            // on the screen now, deleted for this occurrence, next one skipped
bool sched_skip_today(uint32_t id);      // skip the current (or next) occurrence; a one-time schedule is deleted
bool sched_delete_id(uint32_t id);       // delete the whole schedule
bool sched_done(uint32_t id, time_t occ); // "Done" in Telegram: hides that occurrence if it is still on
bool sched_get(uint32_t id, sched_t *out);
void sched_forget_who(uint8_t who);      // a bot member left: their schedules now belong to the owner
