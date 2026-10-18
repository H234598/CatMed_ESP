#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "usb_console.h"
#include "runtime_metrics.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* Compile the actual console and metrics implementations. The hook schedules
 * an SDK-shaped log at the production fwrite/fputc boundary. ESP-IDF 5.5.5's
 * log_format_text.c holds flockfile(stdout) over prefix, body, and newline. */
static FILE *wire;
static pthread_mutex_t schedule_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t schedule_cond = PTHREAD_COND_INITIALIZER;
static bool schedule_log;
static bool log_started;
static bool log_finished;
static bool hook_enabled;
static const char *input_bytes;
static size_t input_position;
static jmp_buf input_finished;

static int scheduled_fgetc(FILE *stream)
{
    (void)stream;
    const char byte = input_bytes[input_position];
    if (byte == '\0') return EOF;
    ++input_position;
    return (unsigned char)byte;
}

static size_t scheduled_fwrite(const void *data, size_t size, size_t count, FILE *stream)
{
    const size_t result = fwrite(data, size, count, stream);
    if (hook_enabled) {
        hook_enabled = false;
        pthread_mutex_lock(&schedule_mutex);
        schedule_log = true;
        pthread_cond_broadcast(&schedule_cond);
        while (!log_started) pthread_cond_wait(&schedule_cond, &schedule_mutex);
        struct timespec until;
        assert(clock_gettime(CLOCK_REALTIME, &until) == 0);
        until.tv_nsec += 200000000L;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_nsec -= 1000000000L;
            ++until.tv_sec;
        }
        while (!log_finished) {
            const int waited = pthread_cond_timedwait(&schedule_cond, &schedule_mutex, &until);
            assert(waited == 0 || waited == ETIMEDOUT);
            if (waited == ETIMEDOUT) break;
        }
        pthread_mutex_unlock(&schedule_mutex);
    }
    return result;
}

static int scheduled_fputs(const char *text, FILE *stream)
{
    const size_t length = strlen(text);
    return scheduled_fwrite(text, 1, length, stream) == length ? 0 : EOF;
}

#undef stdout
#define stdout wire
#define fgetc scheduled_fgetc
#define fwrite scheduled_fwrite
#define fputs scheduled_fputs
#include "../../firmware/main/usb_console.c"
#undef fputs
#undef fwrite
#undef fgetc
#include "../../firmware/main/runtime_metrics.c"
#undef stdout

/* These task/queue stubs are unreachable for a reply-only framing test. */
QueueHandle_t xQueueCreate(unsigned int depth, unsigned int size)
{
    (void)depth;
    (void)size;
    return NULL;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *line, unsigned int ticks)
{
    (void)queue;
    (void)line;
    (void)ticks;
    return 0;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *line, unsigned int ticks)
{
    (void)queue;
    (void)line;
    (void)ticks;
    return 0;
}
void vQueueDelete(QueueHandle_t queue) { (void)queue; }
BaseType_t xTaskCreate(TaskFunction_t task, const char *name, unsigned int stack,
                       void *arg, unsigned int priority, void *handle)
{
    (void)task;
    (void)name;
    (void)stack;
    (void)arg;
    (void)priority;
    (void)handle;
    return 0;
}
void vTaskDelay(unsigned int ticks)
{
    assert(ticks == 20u);
    longjmp(input_finished, 1);
}

void test_esp_log_info(const char *tag, const char *format, ...)
{
    pthread_mutex_lock(&schedule_mutex);
    log_started = true;
    pthread_cond_broadcast(&schedule_cond);
    pthread_mutex_unlock(&schedule_mutex);

    flockfile(wire);
    fprintf(wire, "I (123) %s: ", tag);
    va_list args;
    va_start(args, format);
    vfprintf(wire, format, args);
    va_end(args);
    fputc('\n', wire);
    funlockfile(wire);

    pthread_mutex_lock(&schedule_mutex);
    log_finished = true;
    pthread_cond_broadcast(&schedule_cond);
    pthread_mutex_unlock(&schedule_mutex);
}

static void *log_worker(void *arg)
{
    pthread_mutex_lock(&schedule_mutex);
    while (!schedule_log) pthread_cond_wait(&schedule_cond, &schedule_mutex);
    pthread_mutex_unlock(&schedule_mutex);
    katmed_runtime_metrics_log((const KatmedRuntimeMetricInput *)arg);
    return NULL;
}

static void check_reply_under_log_race(const char *reply)
{
    wire = tmpfile();
    assert(wire != NULL);
    assert(setvbuf(wire, NULL, _IONBF, 0) == 0);
    schedule_log = false;
    log_started = false;
    log_finished = false;
    hook_enabled = true;
    const KatmedRuntimeMetricInput metric = {.uptime_ms = 123};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, log_worker, (void *)&metric) == 0);
    katmed_usb_console_reply(reply);
    assert(pthread_join(thread, NULL) == 0);
    rewind(wire);
    char actual[2048] = {0};
    const size_t length = fread(actual, 1, sizeof(actual) - 1, wire);
    assert(length > 0 && length < sizeof(actual) - 1);
    char expected[2048];
    const int expected_length = snprintf(expected, sizeof(expected), "%s%s", reply,
        reply[0] != '\0' && reply[strlen(reply) - 1] == '\n' ? "" : "\n");
    assert(expected_length > 0 && (size_t)expected_length < sizeof(expected));
    if (strncmp(actual, expected, (size_t)expected_length) != 0) {
        fprintf(stderr, "STATUS line broken at byte %zu: %s\n", strlen(reply), actual);
        assert(!"interleaved protocol line");
    }
    assert(strstr(actual + expected_length, "I (123) katmed_metrics: uptime_ms=123 ") ==
           actual + expected_length);
    assert(actual[length - 1] == '\n');
    fclose(wire);
}

static void check_queue_busy_under_log_race(void)
{
    wire = tmpfile();
    assert(wire != NULL);
    assert(setvbuf(wire, NULL, _IONBF, 0) == 0);
    schedule_log = false;
    log_started = false;
    log_finished = false;
    hook_enabled = true;
    input_bytes = "KM1 PROVISION secret=SYNTHETIC_TEST_SECRET\n";
    input_position = 0;
    input_queue = (QueueHandle_t)1;
    const KatmedRuntimeMetricInput metric = {.uptime_ms = 123};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, log_worker, (void *)&metric) == 0);
    if (setjmp(input_finished) == 0) usb_input_task(NULL);
    assert(input_position == strlen(input_bytes));
    assert(pthread_join(thread, NULL) == 0);
    rewind(wire);
    char actual[1024] = {0};
    const size_t length = fread(actual, 1, sizeof(actual) - 1, wire);
    assert(length > 0 && length < sizeof(actual) - 1);
    assert(strncmp(actual, "KM1 ERR BUSY\nI (123) katmed_metrics: uptime_ms=123 ",
                   strlen("KM1 ERR BUSY\nI (123) katmed_metrics: uptime_ms=123 ")) == 0);
    assert(strstr(actual, "SYNTHETIC_TEST_SECRET") == NULL);
    fclose(wire);
}

int main(void)
{
    check_reply_under_log_race("KM1 OK STATUS configured=0 durable=1 pending=0");
    check_queue_busy_under_log_race();
    char longest[KATMED_USB_OUTPUT_MAX];
    memset(longest, 'X', sizeof(longest) - 1);
    longest[0] = 'K';
    longest[1] = 'M';
    longest[2] = '1';
    longest[3] = ' ';
    longest[sizeof(longest) - 1] = '\0';
    check_reply_under_log_race(longest);

    wire = tmpfile();
    assert(wire != NULL);
    hook_enabled = false;
    katmed_usb_console_reply(NULL);
    char too_long[KATMED_USB_OUTPUT_MAX + 1];
    memset(too_long, 'X', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    katmed_usb_console_reply(too_long);
    assert(ftell(wire) == 0);
    fclose(wire);

    puts("STATUS and SDK diagnostic retain separate complete lines; 255-byte payload and rejection boundary OK");
    return 0;
}
