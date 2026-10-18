#ifndef KATMED_USB_CONSOLE_H
#define KATMED_USB_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include "runtime_metrics.h"

#define KATMED_USB_INPUT_MAX 512u
#define KATMED_USB_OUTPUT_MAX 256u

typedef struct {
    char data[KATMED_USB_INPUT_MAX];
    size_t length;
    bool overlong;
} KatmedUsbLine;

typedef enum {
    KATMED_USB_MAIN_TAKE,
    KATMED_USB_REPLY_READY,
    KATMED_USB_REPLY_CALL,
} KatmedUsbMainPhase;

/* Called once by app_main before start. The callback logs only fixed numeric
 * snapshots; the reader and main task never log while holding the phase lock. */
void katmed_usb_console_phase_init(uint64_t boot_domain,
                                   uint64_t (*uptime_ms)(void),
                                   void (*log_phase)(const KatmedUsbPhase *));
bool katmed_usb_console_is_status(const KatmedUsbLine *line);
void katmed_usb_console_note_main(KatmedUsbMainPhase stage);

/* The input task only frames bytes. The main loop owns Store and executes all
 * commands serially with touch and reducer events. */
bool katmed_usb_console_start(void);
bool katmed_usb_console_receive(KatmedUsbLine *line);
void katmed_usb_console_reply(const char *reply);

#endif
