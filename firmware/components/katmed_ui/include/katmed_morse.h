#ifndef KATMED_MORSE_H
#define KATMED_MORSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A transient, non-blocking optical output. Call update with monotonic time. */
typedef struct {
    size_t byte_index;
    char expansion;
    const char *pattern;
    uint8_t symbol_index;
    uint8_t phase;
    bool active;
    bool white;
    bool paused;
    uint64_t remaining_ms;
    uint64_t due_ms;
} KatmedMorse;

const char *katmed_morse_message(void);
bool katmed_morse_message_valid(void);
void katmed_morse_start(KatmedMorse *morse, uint64_t now_ms);
void katmed_morse_stop(KatmedMorse *morse);
void katmed_morse_pause(KatmedMorse *morse, uint64_t now_ms);
void katmed_morse_resume(KatmedMorse *morse, uint64_t now_ms);
bool katmed_morse_update(KatmedMorse *morse, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
