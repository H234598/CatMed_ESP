#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Reuse the real main/Store host fixture and call its real process_usb. */
#define KATMED_PHASE_HOST_TEST 1
#define main inherited_store_fixture_main
#include "../host_store/check_main_submit.c"
#undef main

#include "freertos/queue.h"
#include "freertos/task.h"

static struct {
    KatmedUsbLine items[3];
    unsigned count, read_index;
} queue;
static TaskFunction_t reader;
static int fail_queue, fail_task;
static const unsigned char *input;
static size_t input_len, input_pos;
static jmp_buf reader_done;
static char wire[1024];
static size_t wire_len;
static char phase_line[768];
static KatmedUsbPhase last_phase;
static unsigned phase_lines;

QueueHandle_t xQueueCreate(unsigned length, unsigned item_size)
{
    assert(length == 3 && item_size == sizeof(KatmedUsbLine));
    if (fail_queue) return NULL;
    memset(&queue, 0, sizeof(queue));
    return &queue;
}
BaseType_t xQueueSend(QueueHandle_t handle, const void *item, unsigned ticks)
{
    assert(handle == &queue && ticks == 0);
    if (queue.count == 3) return 0;
    queue.items[queue.count++] = *(const KatmedUsbLine *)item;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t handle, void *item, unsigned ticks)
{
    assert(handle == &queue && ticks == 0);
    if (queue.read_index == queue.count) return 0;
    *(KatmedUsbLine *)item = queue.items[queue.read_index++];
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t handle) { assert(handle == &queue); }
BaseType_t xTaskCreate(TaskFunction_t task, const char *name, unsigned stack,
                       void *arg, unsigned priority, void *handle)
{
    assert(task && strcmp(name, "katmed_usb") == 0 && stack == 4096);
    assert(arg == NULL && priority == 2 && handle == NULL);
    if (fail_task) return 0;
    reader = task;
    return pdPASS;
}
void vTaskDelay(unsigned ticks)
{
    assert(ticks == 20);
    longjmp(reader_done, 1);
}

static int fake_fgetc(FILE *stream)
{
    (void)stream;
    if (input_pos == input_len) return EOF;
    return input[input_pos++];
}
static size_t fake_fwrite(const void *data, size_t size, size_t count, FILE *stream)
{
    (void)stream;
    const size_t length = size * count;
    assert(wire_len + length < sizeof(wire));
    memcpy(wire + wire_len, data, length);
    wire_len += length;
    wire[wire_len] = 0;
    return count;
}
static int fake_fputs(const char *data, FILE *stream)
{
    (void)stream;
    const size_t length = strlen(data);
    assert(wire_len + length < sizeof(wire));
    memcpy(wire + wire_len, data, length);
    wire_len += length;
    wire[wire_len] = 0;
    return (int)length;
}
static int fake_fputc(int c, FILE *stream)
{
    (void)stream;
    assert(wire_len + 1 < sizeof(wire));
    wire[wire_len++] = (char)c;
    wire[wire_len] = 0;
    return c;
}
static int fake_fflush(FILE *stream) { (void)stream; return 0; }

#define fgetc fake_fgetc
#define fwrite fake_fwrite
#define fputs fake_fputs
#define fputc fake_fputc
#define fflush fake_fflush
#include "../../firmware/main/usb_console.c"
#undef fgetc
#undef fwrite
#undef fputs
#undef fputc
#undef fflush

/* The product formatter is compiled unchanged; only ESP_LOGI's outer device
 * sink is isolated. Its maximum normal prefix is included in the check. */
void test_log_info(const char *tag, const char *format, ...);
#undef ESP_LOGI
#define ESP_LOGI(tag, format, ...) test_log_info(tag, format, ##__VA_ARGS__)
#include "../../firmware/main/runtime_metrics.c"

void test_log_info(const char *tag, const char *format, ...)
{
    assert(strcmp(tag, "katmed_usb_phase") == 0);
    char payload[768];
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(payload, sizeof(payload), format, args);
    va_end(args);
    assert(n > 0 && n < (int)sizeof(payload));
    const int total = snprintf(phase_line, sizeof(phase_line),
                               "I (4294967295) %s: %s", tag, payload);
    assert(total > 0 && total <= 767);
    ++phase_lines;
}

static uint64_t fake_uptime(void) { return (uint64_t)fake_microseconds / 1000; }
static void observe_phase(const KatmedUsbPhase *snapshot)
{
    last_phase = *snapshot;
    katmed_runtime_usb_phase_log(snapshot);
}
static void reset_path(void)
{
    input_queue = NULL;
    reader = NULL;
    fail_queue = fail_task = 0;
    input = NULL;
    input_len = input_pos = 0;
    wire_len = 0;
    wire[0] = 0;
    phase_lines = 0;
    memset(&last_phase, 0, sizeof(last_phase));
    fake_microseconds = 1000000;
    katmed_usb_console_phase_init(UINT64_C(391), fake_uptime, observe_phase);
}
static void feed(const char *bytes)
{
    input = (const unsigned char *)bytes;
    input_len = strlen(bytes);
    input_pos = 0;
    assert(reader);
    if (setjmp(reader_done) == 0) reader(NULL);
    assert(input_pos == input_len);
}

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t description = {.version = "5042056"};
    return &description;
}
int esp_app_get_elf_sha256(char *buffer, size_t size)
{
    assert(size >= sizeof("6c9d77e5b"));
    memcpy(buffer, "6c9d77e5b", sizeof("6c9d77e5b"));
    return 0;
}
const uint8_t *katmed_images_catalog_sha256(void)
{
    static const uint8_t catalog[KATMED_CATALOG_BYTES] = {1, 2, 3};
    return catalog;
}
void katmed_mesh_stop(void) {}
esp_err_t katmed_time_idf_set_root(KatmedTime *time, bool root, bool ip,
                                   const uint8_t *server, size_t length)
{
    (void)time; (void)root; (void)ip; (void)server; (void)length;
    return ESP_OK;
}

int main(void)
{
    reset_path();
    fail_queue = 1;
    assert(!katmed_usb_console_start() && last_phase.task == 2);
    assert(last_phase.read == 0 && last_phase.phase_seq == 1);

    reset_path();
    fail_task = 1;
    assert(!katmed_usb_console_start() && last_phase.task == 2);
    fail_task = 0;
    assert(katmed_usb_console_start() && last_phase.task == 1);
    const uint64_t reentry_seq = last_phase.phase_seq;
    assert(katmed_usb_console_start() && last_phase.phase_seq == reentry_seq);
    feed("KM1 STAT");
    assert(last_phase.read == 1 && last_phase.status == 0);

    reset_path();
    assert(katmed_usb_console_start() && last_phase.task == 1);
    feed("KM1 STATUS\n");
    assert(last_phase.status == 1 && last_phase.queue_ok == 1 &&
           last_phase.queue_drop == 0 && last_phase.main_take == 0);
    initialize_runtime();
    ClockSample now = clock_sample(runtime.boot_domain);
    process_usb(&runtime, &now);
    assert(last_phase.main_take == 1 && last_phase.reply_ready == 1 &&
           last_phase.reply_call == 1);
    assert(strncmp(wire, "KM1 OK STATUS ", 14) == 0);
    assert(strstr(wire, "firmware=5042056 build=6c9d77e5b error=") != NULL);
    const size_t status_len = strcspn(wire, "\n") + 1;
    assert(status_len <= 256);
    unsigned equals = 0;
    for (size_t i = 0; i < status_len; ++i) if (wire[i] == '=') ++equals;
    assert(equals == 9);
    printf("PRODUCT_STATUS=%.*s\n", (int)(status_len - 1), wire);
    printf("PRODUCT_PHASE=%s\n", phase_line);

    /* This calls the real firmware main/Store path with a synthetic group.
     * The same final test bytes must fail behaviorally on the old main. */
    reset_path();
    committed_length=0;
    writes=0;
    initialize_runtime();
    assert(katmed_usb_console_start());
    const uint64_t commit_epoch=runtime.boot.epoch;
    const uint64_t commit_domain=runtime.boot_domain;
    const char *bound_provision=
        "KM1 PROVEBOOT group=11111111111111111111111111111111 "
        "secret=2222222222222222222222222222222222222222222222222222222222222222 "
        "ssid=486f6d65 password=3132333435363738 time=706f6f6c2e6e74702e6f7267\n";
    feed(bound_provision);
    process_usb(&runtime,&now);
    assert(strcmp(wire,"KM1 ERR COMMAND\n")==0 && writes==1);
    wire_len=0; wire[0]=0;
    feed("KM1 ARM n=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n");
    process_usb(&runtime,&now);
    assert(strcmp(wire,"KM1 OK ARM n=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n")==0);
    wire_len=0; wire[0]=0;
    feed(bound_provision);
    process_usb(&runtime,&now);
    assert(strncmp(wire,"KM1 OK PROVEBOOT n=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa dev=",
                   strlen("KM1 OK PROVEBOOT n=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa dev="))==0);
    assert(writes==2 && runtime.store.record.provision.configured &&
           runtime.boot.epoch==commit_epoch && runtime.boot_domain==commit_domain);
    assert(strcspn(wire,"\n")+1<=256);
    reset_path();
    initialize_runtime();
    assert(runtime.boot.epoch>commit_epoch && runtime.boot_domain!=commit_domain);
    assert(katmed_usb_console_start());
    wire_len=0; wire[0]=0;
    feed("KM1 BOOT n=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n");
    ClockSample after=clock_sample(runtime.boot_domain);
    process_usb(&runtime,&after);
    assert(strncmp(wire,"KM1 OK BOOT n=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb c=1 d=1 dev=",
                   strlen("KM1 OK BOOT n=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb c=1 d=1 dev="))==0);
    assert(strcspn(wire,"\n")+1<=256);
    runtime.store.last_error=STORE_STALE;
    wire_len=0; wire[0]=0;
    feed("KM1 BOOT n=cccccccccccccccccccccccccccccccc\n");
    process_usb(&runtime,&after);
    assert(strncmp(wire,"KM1 OK BOOT n=cccccccccccccccccccccccccccccccc c=1 d=0 dev=",
                   strlen("KM1 OK BOOT n=cccccccccccccccccccccccccccccccc c=1 d=0 dev="))==0);

    reset_path();
    assert(katmed_usb_console_start());
    feed("KM1 STATUS\nKM1 STATUS\nKM1 STATUS\nKM1 STATUS\n");
    assert(last_phase.status == 4 && last_phase.queue_ok == 3 &&
           last_phase.queue_drop == 1);
    assert(strstr(wire, "KM1 ERR BUSY\n") != NULL);

    const KatmedUsbPhase worst = {
        .phase_seq=UINT64_MAX,.uptime_ms=UINT64_MAX,
        .boot_domain=UINT64_MAX,.task=2,.read=1,
        .status=UINT32_MAX,.queue_ok=UINT32_MAX,.queue_drop=UINT32_MAX,
        .main_take=UINT32_MAX,.reply_ready=UINT32_MAX,
        .reply_call=UINT32_MAX,
    };
    katmed_runtime_usb_phase_log(&worst);
    assert(strlen(phase_line) <= 767);
    printf("MAX_PHASE_CHARS=%zu SNAPSHOT_BYTES=%zu\n",
           strlen(phase_line), sizeof(KatmedUsbPhase));
    return 0;
}
