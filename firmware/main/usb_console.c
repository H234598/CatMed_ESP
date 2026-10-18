#include "usb_console.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define USB_QUEUE_DEPTH 3u
static QueueHandle_t input_queue;
static KatmedUsbPhase phase;
static uint64_t (*phase_uptime_ms)(void);
static void (*phase_logger)(const KatmedUsbPhase *);
#ifdef ESP_PLATFORM
static portMUX_TYPE phase_lock = portMUX_INITIALIZER_UNLOCKED;
#define PHASE_LOCK() portENTER_CRITICAL(&phase_lock)
#define PHASE_UNLOCK() portEXIT_CRITICAL(&phase_lock)
#else
/* Existing single-threaded host fixtures do not provide the IDF portMUX. */
#define PHASE_LOCK() ((void)0)
#define PHASE_UNLOCK() ((void)0)
#endif

typedef enum {
    PHASE_TASK_OK, PHASE_TASK_FAIL, PHASE_READ,
    PHASE_STATUS, PHASE_QUEUE_OK, PHASE_QUEUE_DROP,
    PHASE_MAIN_TAKE, PHASE_REPLY_READY, PHASE_REPLY_CALL,
} PhaseEvent;

static void increment_saturated(uint32_t *counter)
{
    if (*counter != UINT32_MAX) ++*counter;
}

static void phase_note(PhaseEvent event)
{
    if (phase_uptime_ms == NULL || phase_logger == NULL) return;
    const uint64_t uptime = phase_uptime_ms();
    KatmedUsbPhase snapshot;
    PHASE_LOCK();
    switch (event) {
    case PHASE_TASK_OK: phase.task = 1; break;
    case PHASE_TASK_FAIL: phase.task = 2; break;
    case PHASE_READ:
        if (phase.read != 0) { PHASE_UNLOCK(); return; }
        phase.read = 1;
        break;
    case PHASE_STATUS: increment_saturated(&phase.status); break;
    case PHASE_QUEUE_OK: increment_saturated(&phase.queue_ok); break;
    case PHASE_QUEUE_DROP: increment_saturated(&phase.queue_drop); break;
    case PHASE_MAIN_TAKE: increment_saturated(&phase.main_take); break;
    case PHASE_REPLY_READY: increment_saturated(&phase.reply_ready); break;
    case PHASE_REPLY_CALL: increment_saturated(&phase.reply_call); break;
    }
    if (phase.phase_seq != UINT64_MAX) ++phase.phase_seq;
    phase.uptime_ms = uptime;
    snapshot = phase;
    PHASE_UNLOCK();
    phase_logger(&snapshot);
}

void katmed_usb_console_phase_init(uint64_t boot_domain,
                                   uint64_t (*uptime_ms)(void),
                                   void (*log_phase)(const KatmedUsbPhase *))
{
    PHASE_LOCK();
    phase = (KatmedUsbPhase){.boot_domain = boot_domain};
    phase_uptime_ms = uptime_ms;
    phase_logger = log_phase;
    PHASE_UNLOCK();
}

bool katmed_usb_console_is_status(const KatmedUsbLine *line)
{
    static const char command[] = "KM1 STATUS\n";
    return line != NULL && !line->overlong &&
           line->length == sizeof(command) - 1 &&
           memcmp(line->data, command, sizeof(command) - 1) == 0;
}

void katmed_usb_console_note_main(KatmedUsbMainPhase stage)
{
    switch (stage) {
    case KATMED_USB_MAIN_TAKE: phase_note(PHASE_MAIN_TAKE); break;
    case KATMED_USB_REPLY_READY: phase_note(PHASE_REPLY_READY); break;
    case KATMED_USB_REPLY_CALL: phase_note(PHASE_REPLY_CALL); break;
    }
}

static void usb_input_task(void *arg)
{
    (void)arg;
    KatmedUsbLine line = {0};
    phase_note(PHASE_READ); /* Entered the reader, before any fgetc result. */
    for (;;) {
        const int byte = fgetc(stdin);
        if (byte == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!line.overlong) {
            if (line.length < sizeof(line.data)) {
                line.data[line.length++] = (char)byte;
            } else {
                line.overlong = true;
            }
        }
        if (byte != '\n') continue;
        const bool is_status = katmed_usb_console_is_status(&line);
        if (is_status) phase_note(PHASE_STATUS);
        const bool sent = xQueueSend(input_queue, &line, 0) == pdTRUE;
        if (is_status) phase_note(sent ? PHASE_QUEUE_OK : PHASE_QUEUE_DROP);
        if (!sent) {
            /* No input or credential content is echoed, including on overflow. */
            flockfile(stdout);
            fputs("KM1 ERR BUSY\n", stdout);
            fflush(stdout);
            funlockfile(stdout);
        }
        memset(&line, 0, sizeof(line));
    }
}

bool katmed_usb_console_start(void)
{
    if (input_queue != NULL) return true;
    input_queue = xQueueCreate(USB_QUEUE_DEPTH, sizeof(KatmedUsbLine));
    if (input_queue == NULL) { phase_note(PHASE_TASK_FAIL); return false; }
    if (xTaskCreate(usb_input_task, "katmed_usb", 4096, NULL, 2, NULL) != pdPASS) {
        vQueueDelete(input_queue);
        input_queue = NULL;
        phase_note(PHASE_TASK_FAIL);
        return false;
    }
    phase_note(PHASE_TASK_OK);
    return true;
}

bool katmed_usb_console_receive(KatmedUsbLine *line)
{
    return input_queue != NULL && line != NULL &&
           xQueueReceive(input_queue, line, 0) == pdTRUE;
}

void katmed_usb_console_reply(const char *reply)
{
    if (reply == NULL) return;
    const size_t length = strnlen(reply, KATMED_USB_OUTPUT_MAX);
    if (length == KATMED_USB_OUTPUT_MAX) return;
    /* ESP-IDF's formatted logs hold this same stream lock for their entire
     * line. Keep the protocol payload, newline, and flush inside one lock. */
    flockfile(stdout);
    fwrite(reply, 1, length, stdout);
    if (length == 0 || reply[length - 1] != '\n') fputc('\n', stdout);
    fflush(stdout);
    funlockfile(stdout);
}
