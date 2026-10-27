// Panels on the same network share messages and new schedules (experimental).
// One UDP socket, broadcast on the local subnet, no TLS: every packet is signed with HMAC-SHA256 keyed
// by the shared password, so only panels with the same password accept it. Small packets, one task.
//
// Packet: "TSB1" | type (1) | seq (4, BE) | sender MAC (6) | payload | HMAC (16, truncated)
//   HELLO  payload: version string                       every HELLO_S, "I'm here"
//   MSG    payload: id (4) | from (24) | text            a direct message (from Telegram) to show
//   SCHED  payload: id (4) | sched_t                     a new schedule
//   ACK    payload: type (1) | id (4)                    received, for every copy (the sender's stats)
//   STATE  payload: text lines, tab-separated             what the screen shows, for mirrors (the desktop
//          W temp*10 code max min rain day  /  S name temp*10 rh trend  /  V vent  /  U unread
//          M unread when from text  /  N start text       widget); sent on change and every STATE_S
//
// The password is the pairing code (NVS "lan_code"), made here and shown on the setup page.
#include "app.h"
#include "config.h"
#include "schedule.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "psa/crypto.h"

#define LAN_PORT        47474
#define HELLO_S         10
#define STATS_S         30
#define STATE_S         30      // state for the mirrors at least this often, and when it changes (checked every 5 s)
#define STATE_CHECK_S   5
#define STATE_MAX       1024
#define STATE_MSGS      2
#define STATE_SENSORS   4
#define START_DELAY_S   20      // queue and task only after the boot's TLS (weather, Telegram): the heap's low point
#define PEERS_MAX       8
#define SEEN_MAX        32
#define REPEAT          3       // each message is broadcast this many times (UDP may drop one)
#define REPEAT_MS       300
#define HDR_LEN         15
#define MAC_LEN         16
#define PKT_MAX         (HDR_LEN + 4 + MSG_FROM + MSG_TEXT + MAC_LEN)

enum { T_HELLO = 1, T_MSG, T_SCHED, T_ACK, T_STATE };

static const char *TAG = "lan";
static uint8_t s_key[32];       // SHA-256 of the password
static uint8_t s_mac[6];
static uint32_t s_seq;
static int s_sock = -1;
static QueueHandle_t s_out;     // packets to broadcast (built by other tasks)
static struct {
    uint8_t mac[6];
    int64_t seen;               // monotonic s
} s_peers[PEERS_MAX];
static struct {
    uint8_t mac[6];
    uint8_t type;
    uint32_t id;
} s_seen[SEEN_MAX];             // ring: messages already taken (each comes REPEAT times)
static int s_seen_i;
static bool s_started;
static uint32_t s_rx, s_rx_bad, s_tx;

typedef struct {
    uint16_t len;
    uint8_t buf[PKT_MAX];
} pkt_t;
static pkt_t s_tx_pkt;          // the lan task's own outgoing packet (ACK, HELLO, one from the queue): one at a time

static void sha256(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen, uint8_t out[32])
{
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    size_t n;
    psa_hash_setup(&op, PSA_ALG_SHA_256);
    psa_hash_update(&op, a, alen);
    if (b) {
        psa_hash_update(&op, b, blen);
    }
    psa_hash_finish(&op, out, 32, &n);
}

// HMAC-SHA256 (RFC 2104) with the 32-byte key, truncated to MAC_LEN
static void hmac(const uint8_t *data, size_t len, uint8_t out[MAC_LEN])
{
    uint8_t pad[64], inner[32], full[32];
    for (int i = 0; i < 64; i++) {
        pad[i] = (i < 32 ? s_key[i] : 0) ^ 0x36;
    }
    sha256(pad, 64, data, len, inner);
    for (int i = 0; i < 64; i++) {
        pad[i] = (i < 32 ? s_key[i] : 0) ^ 0x5c;
    }
    sha256(pad, 64, inner, 32, full);
    memcpy(out, full, MAC_LEN);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int64_t mono_s(void)
{
    return esp_timer_get_time() / 1000000;
}

// header + payload + signature into pkt
static void build(pkt_t *pkt, uint8_t type, const void *payload, size_t plen)
{
    memcpy(pkt->buf, "TSB1", 4);
    pkt->buf[4] = type;
    put32(pkt->buf + 5, ++s_seq);
    memcpy(pkt->buf + 9, s_mac, 6);
    memcpy(pkt->buf + HDR_LEN, payload, plen);
    hmac(pkt->buf, HDR_LEN + plen, pkt->buf + HDR_LEN + plen);
    pkt->len = HDR_LEN + plen + MAC_LEN;
}

static void send_now(const pkt_t *pkt)
{
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(LAN_PORT), .sin_addr.s_addr = INADDR_BROADCAST};
    if (sendto(s_sock, pkt->buf, pkt->len, 0, (struct sockaddr *)&to, sizeof(to)) > 0) {
        s_tx++;
    }
}

static void queue(uint8_t type, const void *payload, size_t plen)
{
    if (!s_out) {
        return;
    }
    static pkt_t pkt;   // callers are the bot / the setup page, one at a time
    build(&pkt, type, payload, plen);
    xQueueSend(s_out, &pkt, 0);
}

void lan_share_msg(const char *from, const char *text)
{
    uint8_t p[4 + MSG_FROM + MSG_TEXT] = {0};
    put32(p, esp_random());   // only spots the repeats
    strlcpy((char *)p + 4, from ? from : "", MSG_FROM);
    size_t tl = strnlen(text, MSG_TEXT - 1);
    memcpy(p + 4 + MSG_FROM, text, tl);
    queue(T_MSG, p, 4 + MSG_FROM + tl);
}

void lan_share_sched(const sched_t *s)
{
    uint8_t p[4 + sizeof(sched_t)];
    put32(p, esp_random());   // the receivers number it themselves; this only spots the repeats
    memcpy(p + 4, s, sizeof(sched_t));
    queue(T_SCHED, p, sizeof(p));
}

static bool seen(const uint8_t *mac, uint8_t type, uint32_t id)
{
    for (int i = 0; i < SEEN_MAX; i++) {
        if (s_seen[i].type == type && s_seen[i].id == id && memcmp(s_seen[i].mac, mac, 6) == 0) {
            return true;
        }
    }
    memcpy(s_seen[s_seen_i].mac, mac, 6);
    s_seen[s_seen_i].type = type;
    s_seen[s_seen_i].id = id;
    s_seen_i = (s_seen_i + 1) % SEEN_MAX;
    return false;
}

static void peer_seen(const uint8_t *mac)
{
    int free_i = -1, oldest = 0;
    for (int i = 0; i < PEERS_MAX; i++) {
        if (memcmp(s_peers[i].mac, mac, 6) == 0) {
            s_peers[i].seen = mono_s();
            return;
        }
        if (!s_peers[i].seen && free_i < 0) {
            free_i = i;
        }
        if (s_peers[i].seen < s_peers[oldest].seen) {
            oldest = i;
        }
    }
    int i = free_i >= 0 ? free_i : oldest;
    memcpy(s_peers[i].mac, mac, 6);
    s_peers[i].seen = mono_s();
    ESP_LOGI(TAG, "new panel on the network (%02x:%02x)", mac[4], mac[5]);
}

int lan_peers(void)
{
    int n = 0;
    for (int i = 0; i < PEERS_MAX; i++) {
        n += s_peers[i].seen && mono_s() - s_peers[i].seen <= 3 * HELLO_S;
    }
    return n;
}

static void handle(const uint8_t *buf, int len)
{
    if (len < HDR_LEN + MAC_LEN || memcmp(buf, "TSB1", 4) != 0 || memcmp(buf + 9, s_mac, 6) == 0) {
        return;   // not ours, or our own broadcast coming back
    }
    if (buf[4] == T_STATE) {
        return;   // for the mirrors (and bigger than this buffer)
    }
    uint8_t mac[MAC_LEN];
    hmac(buf, len - MAC_LEN, mac);
    if (memcmp(mac, buf + len - MAC_LEN, MAC_LEN) != 0) {
        s_rx_bad++;   // another password, or damaged
        return;
    }
    s_rx++;
    const uint8_t *from = buf + 9, *p = buf + HDR_LEN;
    int plen = len - HDR_LEN - MAC_LEN;
    peer_seen(from);
    int64_t t0 = esp_timer_get_time();
    switch (buf[4]) {
    case T_MSG: {
        if (plen < 4 + MSG_FROM + 1) {
            break;
        }
        uint8_t ack[5] = {T_MSG};
        put32(ack + 1, get32(p));
        pkt_t *pkt = &s_tx_pkt;
        build(pkt, T_ACK, ack, sizeof(ack));
        // for every copy (a broadcast has no Wi-Fi retries). Not unicast: some routers drop unicast
        // between their 2.4 and 5 GHz networks but pass broadcasts
        send_now(pkt);
        if (seen(from, T_MSG, get32(p))) {
            break;
        }
        char name[MSG_FROM], text[MSG_TEXT];
        memcpy(name, p + 4, MSG_FROM);
        name[MSG_FROM - 1] = '\0';
        int tl = plen - 4 - MSG_FROM;
        tl = tl >= MSG_TEXT ? MSG_TEXT - 1 : tl;
        memcpy(text, p + 4 + MSG_FROM, tl);
        text[tl] = '\0';
        msg_add_direct(text, name);   // not shared again: only messages from the bot are
        ESP_LOGI(TAG, "message from the network shown in %" PRId64 " us, heap %u KB free, %u KB min",
                 esp_timer_get_time() - t0, (unsigned)(esp_get_free_heap_size() / 1024),
                 (unsigned)(esp_get_minimum_free_heap_size() / 1024));
        break;
    }
    case T_SCHED: {
        if (plen != 4 + (int)sizeof(sched_t) || seen(from, T_SCHED, get32(p))) {
            break;
        }
        sched_t s;
        memcpy(&s, p + 4, sizeof(s));
        s.text[SCHED_TEXT - 1] = '\0';
        s.who = 0;   // ours now; the Telegram reminder goes to this panel's owner
        bool ok = sched_add(&s);
        ESP_LOGI(TAG, "schedule from the network %s", ok ? "added" : "dropped (full)");
        break;
    }
    default:
        break;
    }
}

// field without tabs or line breaks (they separate the fields)
static int field(char *out, size_t size, const char *in)
{
    size_t n = 0;
    for (; *in && n + 1 < size; in++) {
        out[n++] = *in == '\t' || *in == '\n' || *in == '\r' ? ' ' : *in;
    }
    out[n] = '\0';
    return n;
}

typedef struct {
    weather_t wx;
    govee_sensor_t sens[STATE_SENSORS];
    msg_item_t msgs[STATE_MSGS];
    char name[MSG_FROM], text[SCHED_TEXT];
    uint8_t pkt[STATE_MAX];
} state_buf_t;

// what the screen shows, as text lines; on the heap only while it's built and sent (~3 KB)
static void send_state(bool force)
{
    static uint32_t last_sum;
    static int64_t last_at;
    state_buf_t *b = malloc(sizeof(*b));
    if (!b) {
        return;
    }
    char *o = (char *)b->pkt + HDR_LEN;   // the text goes straight after the header
    size_t cap = STATE_MAX - HDR_LEN - MAC_LEN, n = 0;
#define ADD(...) do { int k = snprintf(o + n, cap - n, __VA_ARGS__); n = k > 0 && n + k < cap ? n + k : n; } while (0)
    if (net_get_weather(&b->wx)) {
        int rain = b->wx.n_rain ? b->wx.rain_pct[0] : -1;
        ADD("W\t%d\t%d\t%d\t%d\t%d\t%d\n", (int)lroundf(b->wx.temp * 10), b->wx.code,
            b->wx.n_days ? (int)lroundf(b->wx.days[0].temp_max) : 0, b->wx.n_days ? (int)lroundf(b->wx.days[0].temp_min) : 0,
            rain, dimmer_is_day());
    }
    int ns = govee_get(b->sens, STATE_SENSORS, false);
    for (int i = 0; i < ns; i++) {
        field(b->name, sizeof(b->name), b->sens[i].name);
        ADD("S\t%s\t%d\t%d\t%d\n", b->name, (int)lroundf(b->sens[i].temp_c * 10), (int)lroundf(b->sens[i].humidity),
            b->sens[i].has_trend ? b->sens[i].trend : 0);
    }
    for (int i = 0; i < GOVEE_CMP_MAX; i++) {
        int v = govee_vent(i);
        if (v >= 0) {
            ADD("V\t%d\n", v);
            break;
        }
    }
    ADD("U\t%d\n", msg_unread_count());
    int nm = msg_active(b->msgs, STATE_MSGS);
    for (int i = 0; i < nm; i++) {
        field(b->name, sizeof(b->name), b->msgs[i].from);
        field(b->text, sizeof(b->text), b->msgs[i].text);
        ADD("M\t%d\t%lld\t%s\t%s\n", b->msgs[i].unread, (long long)b->msgs[i].when, b->name, b->text);
    }
    time_t when;
    if (sched_next(b->text, sizeof(b->text), &when)) {
        field(b->text, sizeof(b->text), b->text);
        ADD("N\t%lld\t%s\n", (long long)when, b->text);
    }
#undef ADD
    uint32_t sum = 2166136261u;   // FNV-1a: send only when something changed
    for (size_t i = 0; i < n; i++) {
        sum = (sum ^ (uint8_t)o[i]) * 16777619u;
    }
    if (force || sum != last_sum || mono_s() - last_at >= STATE_S) {
        last_sum = sum;
        last_at = mono_s();
        uint8_t *pk = b->pkt;   // build() is for pkt_t, sized for messages: same header, bigger buffer
        memcpy(pk, "TSB1", 4);
        pk[4] = T_STATE;
        put32(pk + 5, ++s_seq);
        memcpy(pk + 9, s_mac, 6);
        hmac(pk, HDR_LEN + n, pk + HDR_LEN + n);
        struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(LAN_PORT), .sin_addr.s_addr = INADDR_BROADCAST};
        if (sendto(s_sock, pk, HDR_LEN + n + MAC_LEN, 0, (struct sockaddr *)&to, sizeof(to)) > 0) {
            s_tx++;
        }
    }
    free(b);
}

static void lan_task(void *arg)
{
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int yes = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct timeval tv = {.tv_sec = 0, .tv_usec = 200 * 1000};
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(LAN_PORT), .sin_addr.s_addr = INADDR_ANY};
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "no UDP port");
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "on, port %d", LAN_PORT);
    static uint8_t buf[PKT_MAX + 16];
    int64_t hello_at = 0, stats_at = mono_s(), state_at = 0;
    while (true) {
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, NULL, NULL);
        if (n > 0) {
            handle(buf, n);
        }
        // outgoing: each one REPEAT times, REPEAT_MS apart (receivers drop the copies)
        if (xQueueReceive(s_out, &s_tx_pkt, 0) == pdTRUE) {
            for (int r = 0; r < REPEAT; r++) {
                send_now(&s_tx_pkt);
                if (r + 1 < REPEAT) {
                    vTaskDelay(pdMS_TO_TICKS(REPEAT_MS));
                }
            }
        }
        int64_t now = mono_s();
        if (now - hello_at >= HELLO_S) {
            hello_at = now;
            const char *ver = esp_app_get_description()->version;
            build(&s_tx_pkt, T_HELLO, ver, strlen(ver));
            send_now(&s_tx_pkt);
        }
        if (now - state_at >= STATE_CHECK_S) {
            state_at = now;
            send_state(false);
        }
        if (now - stats_at >= STATS_S) {
            stats_at = now;
            ESP_LOGI(TAG, "peers %d, rx %" PRIu32 " (bad %" PRIu32 "), tx %" PRIu32 ", heap %u KB free, %u KB min, "
                     "largest %u KB, stack left %u",
                     lan_peers(), s_rx, s_rx_bad, s_tx, (unsigned)(esp_get_free_heap_size() / 1024),
                     (unsigned)(esp_get_minimum_free_heap_size() / 1024),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
    }
}

static void lan_begin(void *arg)
{
    s_out = xQueueCreate(4, sizeof(pkt_t));
    if (!s_out || xTaskCreate(lan_task, "lan", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory to start");
    }
}

// Pairing code: 8 letters and digits without the look-alikes (0/O, 1/I/L), shown as XXXX-XXXX
static const char CODE_ABC[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";

void lan_code_make(char out[LAN_CODE_SIZE])
{
    for (int i = 0; i < LAN_CODE_SIZE - 1; i++) {
        out[i] = i == 4 ? '-' : CODE_ABC[esp_random() % (sizeof(CODE_ABC) - 1)];
    }
    out[LAN_CODE_SIZE - 1] = '\0';
}

bool lan_code_set(const char *code)
{
    if (strlen(code) != LAN_CODE_SIZE - 1) {
        return false;
    }
    for (int i = 0; i < LAN_CODE_SIZE - 1; i++) {
        if (i == 4 ? code[i] != '-' : !strchr(CODE_ABC, code[i])) {
            return false;
        }
    }
    config_set_str("lan_code", code);
    ESP_LOGI(TAG, "new pairing code");
    return true;
}

// made the first time it's asked for (the setup page shows it before sharing is on: unused until then)
void lan_code_get(char *out, size_t size)
{
    config_get_str("lan_code", out, size);
    if (!out[0]) {
        char code[LAN_CODE_SIZE];
        lan_code_make(code);
        lan_code_set(code);
        strlcpy(out, code, size);
    }
}

void lan_start(const char *password)
{
    if (!password || !password[0] || s_started) {
        return;
    }
    s_started = true;
    psa_crypto_init();   // already done by ESP-IDF; harmless if so
    sha256((const uint8_t *)password, strlen(password), NULL, 0, s_key);
    esp_read_mac(s_mac, ESP_MAC_WIFI_STA);
    const esp_timer_create_args_t args = {.callback = lan_begin, .name = "lan"};
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, START_DELAY_S * 1000000LL);
    }
}
