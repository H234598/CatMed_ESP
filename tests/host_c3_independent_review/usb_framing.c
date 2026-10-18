#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "usb_console.h"
#include "runtime_metrics.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* ESP-IDF 5.5.5 log_format_text.c locks stdout around formatted lines.
 * Put the production USB fwrite/fputs return boundary under scheduler control. */
static FILE *capture;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static bool start_log, log_attempted, log_completed, inject;
static const char *input;
static size_t input_offset;
static jmp_buf input_done;

static void schedule_diagnostic(void)
{
    if (!inject) return;
    inject = false;
    assert(pthread_mutex_lock(&mutex) == 0);
    start_log = true;
    assert(pthread_cond_broadcast(&cond) == 0);
    while (!log_attempted) assert(pthread_cond_wait(&cond, &mutex) == 0);
    /* With the old code the log finishes here; with the fix it must wait for
     * the already-held stdout lock until the KM1 newline and flush finish. */
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_nsec += 200000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        ++deadline.tv_sec;
    }
    while (!log_completed) {
        int rc = pthread_cond_timedwait(&cond, &mutex, &deadline);
        assert(rc == 0 || rc == ETIMEDOUT);
        if (rc == ETIMEDOUT) break;
    }
    assert(pthread_mutex_unlock(&mutex) == 0);
}

static size_t intercepted_fwrite(const void *ptr, size_t size, size_t count, FILE *stream)
{
    size_t result = fwrite(ptr, size, count, stream);
    schedule_diagnostic();
    return result;
}

static int intercepted_fputs(const char *text, FILE *stream)
{
    int result = fputs(text, stream);
    schedule_diagnostic();
    return result;
}

static int scripted_fgetc(FILE *stream)
{
    (void)stream;
    if (input[input_offset] == '\0') return EOF;
    return (unsigned char)input[input_offset++];
}

#undef stdout
#define stdout capture
#define fwrite intercepted_fwrite
#define fputs intercepted_fputs
#define fgetc scripted_fgetc
#include KATMED_USB_SOURCE
#undef fgetc
#undef fputs
#undef fwrite
#undef stdout

#include "runtime_metrics.c"

QueueHandle_t xQueueCreate(unsigned int length, unsigned int item_size)
{ (void)length; (void)item_size; return NULL; }
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, unsigned int ticks)
{ (void)queue; (void)item; (void)ticks; return 0; }
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, unsigned int ticks)
{ (void)queue; (void)item; (void)ticks; return 0; }
void vQueueDelete(QueueHandle_t queue) { (void)queue; }
BaseType_t xTaskCreate(TaskFunction_t task, const char *name, unsigned int stack,
                       void *arg, unsigned int priority, void *task_handle)
{ (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)task_handle; return 0; }
void vTaskDelay(unsigned int ticks)
{ assert(ticks == 20); longjmp(input_done, 1); }

void independent_sdk_log_info(const char *tag, const char *format, ...)
{
    assert(pthread_mutex_lock(&mutex) == 0);
    log_attempted = true;
    assert(pthread_cond_broadcast(&cond) == 0);
    assert(pthread_mutex_unlock(&mutex) == 0);

    flockfile(capture);
    fprintf(capture, "I (123) %s: ", tag);
    va_list arguments;
    va_start(arguments, format);
    vfprintf(capture, format, arguments);
    va_end(arguments);
    fputc('\n', capture);
    funlockfile(capture);

    assert(pthread_mutex_lock(&mutex) == 0);
    log_completed = true;
    assert(pthread_cond_broadcast(&cond) == 0);
    assert(pthread_mutex_unlock(&mutex) == 0);
}

static void *diagnostic_thread(void *arg)
{
    assert(pthread_mutex_lock(&mutex) == 0);
    while (!start_log) assert(pthread_cond_wait(&cond, &mutex) == 0);
    assert(pthread_mutex_unlock(&mutex) == 0);
    katmed_runtime_metrics_log(arg);
    return NULL;
}

static void test_case(bool busy)
{
    capture = tmpfile();
    assert(capture != NULL);
    assert(setvbuf(capture, NULL, _IONBF, 0) == 0);
    start_log = log_attempted = log_completed = false;
    inject = true;
    const KatmedRuntimeMetricInput metrics = {.uptime_ms = 123};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, diagnostic_thread, (void *)&metrics) == 0);
    const char *first_line = busy ? "KM1 ERR BUSY\n" :
        "KM1 OK STATUS configured=0 durable=1 pending=0\n";
    if (busy) {
        input = "KM1 PROVISION secret=INDEPENDENT_TEST_SECRET\n";
        input_offset = 0;
        input_queue = (QueueHandle_t)1;
        if (setjmp(input_done) == 0) usb_input_task(NULL);
        assert(input_offset == strlen(input));
    } else {
        katmed_usb_console_reply("KM1 OK STATUS configured=0 durable=1 pending=0");
    }
    assert(pthread_join(thread, NULL) == 0);
    rewind(capture);
    char output[2048] = {0};
    size_t actual = fread(output, 1, sizeof(output) - 1, capture);
    assert(actual > strlen(first_line));
    const size_t first_length = strlen(first_line);
    const char *log_start = "I (123) katmed_metrics: uptime_ms=123 ";
    if (memcmp(output, first_line, first_length) != 0 ||
        strncmp(output + first_length, log_start, strlen(log_start)) != 0 ||
        strstr(output, "INDEPENDENT_TEST_SECRET") != NULL || output[actual - 1] != '\n') {
        fprintf(stderr, "broken %s wire lines: %s\n", busy ? "BUSY" : "STATUS", output);
        return abort();
    }
    fclose(capture);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "status") == 0) test_case(false);
    else if (strcmp(argv[1], "busy") == 0) test_case(true);
    else abort();
    printf("independent %s versus SDK metrics framing: OK\n", argv[1]);
    return 0;
}
