#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../firmware/main/usb_console.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static struct {
    KatmedUsbLine items[3];
    unsigned int count;
    unsigned int read_index;
} fake_queue;
static TaskFunction_t input_task;
static const unsigned char *input_bytes;
static size_t input_length;
static size_t input_position;
static char output[256];
static size_t output_length;
static jmp_buf input_complete;

QueueHandle_t xQueueCreate(unsigned int length, unsigned int item_size)
{
    assert(length == 3u);
    assert(item_size == sizeof(KatmedUsbLine));
    memset(&fake_queue, 0, sizeof(fake_queue));
    return &fake_queue;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, unsigned int ticks)
{
    assert(queue == &fake_queue && ticks == 0u);
    if (fake_queue.count == 3u) return 0;
    fake_queue.items[fake_queue.count++] = *(const KatmedUsbLine *)item;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, unsigned int ticks)
{
    assert(queue == &fake_queue && ticks == 0u);
    if (fake_queue.read_index == fake_queue.count) return 0;
    *(KatmedUsbLine *)item = fake_queue.items[fake_queue.read_index++];
    return pdTRUE;
}

void vQueueDelete(QueueHandle_t queue) { assert(queue == &fake_queue); }

BaseType_t xTaskCreate(TaskFunction_t task, const char *name, unsigned int stack,
                       void *arg, unsigned int priority, void *task_handle)
{
    assert(task != NULL && strcmp(name, "katmed_usb") == 0);
    assert(stack == 4096u && arg == NULL && priority == 2u && task_handle == NULL);
    input_task = task;
    return pdPASS;
}

void vTaskDelay(unsigned int ticks)
{
    assert(ticks == 20u);
    longjmp(input_complete, 1);
}

static int harness_fgetc(FILE *unused)
{
    (void)unused;
    if (input_position == input_length) return EOF;
    return input_bytes[input_position++];
}

static int harness_fputs(const char *text, FILE *unused)
{
    (void)unused;
    size_t length = strlen(text);
    assert(output_length + length < sizeof(output));
    memcpy(output + output_length, text, length);
    output_length += length;
    output[output_length] = '\0';
    return (int)length;
}

static size_t harness_fwrite(const void *bytes, size_t size, size_t count, FILE *unused)
{
    (void)unused;
    size_t length = size * count;
    assert(output_length + length < sizeof(output));
    memcpy(output + output_length, bytes, length);
    output_length += length;
    output[output_length] = '\0';
    return count;
}

static int harness_fputc(int c, FILE *unused)
{
    (void)unused;
    assert(output_length + 1u < sizeof(output));
    output[output_length++] = (char)c;
    output[output_length] = '\0';
    return c;
}

static int harness_fflush(FILE *unused) { (void)unused; return 0; }

/* Compile the production framing code itself against bounded task/queue and
 * input/output stubs. No duplicate framing implementation exists here. */
#define fgetc harness_fgetc
#define fputs harness_fputs
#define fwrite harness_fwrite
#define fputc harness_fputc
#define fflush harness_fflush
#include "../../firmware/main/usb_console.c"
#undef fgetc
#undef fputs
#undef fwrite
#undef fputc
#undef fflush

static void run_input(const unsigned char *bytes, size_t length)
{
    input_bytes = bytes;
    input_length = length;
    input_position = 0u;
    if (setjmp(input_complete) == 0) input_task(NULL);
    assert(input_position == input_length);
}

static void reset_console(void)
{
    input_queue = NULL;
    input_task = NULL;
    output_length = 0u;
    output[0] = '\0';
    assert(katmed_usb_console_start());
    assert(input_task != NULL);
}

static void test_exact_limit_and_overflow_recovery(void)
{
    unsigned char bytes[512u + 1u + sizeof("KM1 STATUS\n")];
    KatmedUsbLine line;
    reset_console();
    memset(bytes, 'a', 511u);
    bytes[511u] = '\n';
    run_input(bytes, 512u);
    assert(katmed_usb_console_receive(&line));
    assert(line.length == 512u && !line.overlong && line.data[511u] == '\n');
    assert(!katmed_usb_console_receive(&line));

    reset_console();
    memset(bytes, 'b', 512u);
    bytes[512u] = '\n';
    memcpy(bytes + 513u, "KM1 STATUS\n", sizeof("KM1 STATUS\n") - 1u);
    run_input(bytes, 513u + sizeof("KM1 STATUS\n") - 1u);
    assert(katmed_usb_console_receive(&line));
    assert(line.length == 512u && line.overlong);
    assert(katmed_usb_console_receive(&line));
    assert(line.length == sizeof("KM1 STATUS\n") - 1u && !line.overlong);
    assert(memcmp(line.data, "KM1 STATUS\n", line.length) == 0);
    assert(!katmed_usb_console_receive(&line));
    assert(output_length == 0u);
}

static void test_full_queue_does_not_echo_input(void)
{
    const unsigned char bytes[] =
        "KM1 STATUS\nKM1 STATUS\nKM1 STATUS\n"
        "KM1 PROVISION secret=SYNTHETIC_TEST_SECRET\n";
    KatmedUsbLine line;
    reset_console();
    run_input(bytes, sizeof(bytes) - 1u);
    assert(fake_queue.count == 3u);
    assert(strcmp(output, "KM1 ERR BUSY\n") == 0);
    assert(strstr(output, "SYNTHETIC_TEST_SECRET") == NULL);
    for (unsigned int i = 0; i < 3u; ++i) {
        assert(katmed_usb_console_receive(&line));
        assert(line.length == sizeof("KM1 STATUS\n") - 1u && !line.overlong);
    }
    assert(!katmed_usb_console_receive(&line));
}

int main(void)
{
    test_exact_limit_and_overflow_recovery();
    test_full_queue_does_not_echo_input();
    puts("actual usb_console framing: 512-byte boundary, recovery, queue full, no input echo OK");
    return 0;
}
