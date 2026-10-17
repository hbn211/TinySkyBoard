#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Settings stored in NVS. The first time, the menuconfig values are used.
typedef struct {
    char ssid[33];
    char pass[65];
    char city[48];
    char lat[16];   // empty = look it up from the city (geocoding)
    char lon[16];
    char tz[48];    // POSIX TZ; updated automatically from the offset Open-Meteo returns
    char tg_token[80];  // Telegram bot token; empty = disabled
    int64_t tg_owner;
    char govee_key[48];  // Govee OpenAPI key; empty = no sensors
    char cities[128];
    bool h12;            // 12-hour clock with AM/PM (NVS "h12"); false = 24 h    // other cities, comma-separated; empty = none   // owner's chat_id; 0 = the first chat to talk to the bot becomes the owner
} config_t;

extern config_t g_cfg;

void config_load(void);
void clock_fmt(char *out, size_t size, int hour, int min);   // "14:05" or "2:05 PM" (g_cfg.h12)
void config_save(void);
bool config_split_city(void);   // "A, B" in the main city -> main "A", B goes to the other cities

uint8_t config_get_u8(const char *key);
uint8_t config_get_u8_def(const char *key, uint8_t def);   // def if never saved
void config_set_u8(const char *key, uint8_t val);

// Requests setup mode on the next boot and restarts
void config_request_portal(void);
// true if this boot should open the portal (consumes the request)
bool config_take_portal_request(void);
