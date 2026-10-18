// Where the last crash happened, kept across the restart (memory not cleared at boot), so the bot's
// power-on notice can say it. esp_panic_handler is wrapped at link time (main/CMakeLists.txt).
#include "app.h"

#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_private/panic_internal.h"
#include "riscv/rvruntime-frames.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CRASH_MAGIC 0x43524153u   // "CRAS"

typedef struct {
    uint32_t magic;
    uint32_t pc, ra;
    char task[16];
    char why[72];
} crash_t;

static RTC_NOINIT_ATTR crash_t s_crash;

extern bool g_panic_abort;
extern char *g_panic_abort_details;
void __real_esp_panic_handler(panic_info_t *info);

void __wrap_esp_panic_handler(panic_info_t *info)
{
    s_crash.magic = CRASH_MAGIC;
    s_crash.pc = (uint32_t)info->addr;
    s_crash.ra = info->frame ? ((const RvExcFrame *)info->frame)->ra : 0;
    const char *task = xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED ? "-" : pcTaskGetName(NULL);
    strlcpy(s_crash.task, task ? task : "?", sizeof(s_crash.task));
    const char *why = g_panic_abort && g_panic_abort_details ? g_panic_abort_details : info->reason;
    strlcpy(s_crash.why, why ? why : "?", sizeof(s_crash.why));
    __real_esp_panic_handler(info);
}

bool crash_take(char *out, size_t size)
{
    bool ok = s_crash.magic == CRASH_MAGIC && esp_reset_reason() == ESP_RST_PANIC;
    if (ok) {
        s_crash.why[sizeof(s_crash.why) - 1] = '\0';
        s_crash.task[sizeof(s_crash.task) - 1] = '\0';
        snprintf(out, size, "%s \xC2\xB7 %s \xC2\xB7 PC 0x%08lx RA 0x%08lx \xC2\xB7 v%s", s_crash.why, s_crash.task,
                 (unsigned long)s_crash.pc, (unsigned long)s_crash.ra, esp_app_get_description()->version);
    }
    s_crash.magic = 0;
    return ok;
}
