#include "katmed_morse.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    const char *message = katmed_morse_message();
    assert(message && message[0]);
    assert(katmed_morse_message_valid());

    KatmedMorse morse;
    const uint64_t start_ms = UINT64_C(12345);
    katmed_morse_start(&morse, start_ms);
    assert(morse.active);
    assert(morse.white && morse.due_ms > start_ms);
    const uint64_t first_mark_end = morse.due_ms;
    assert(katmed_morse_update(&morse, first_mark_end - 1));
    assert(!katmed_morse_update(&morse, first_mark_end));
    assert(morse.phase == 1 && !morse.white && morse.due_ms > first_mark_end);
    const uint64_t next_mark_start = morse.due_ms;
    assert(!katmed_morse_update(&morse, next_mark_start - 1));
    assert(katmed_morse_update(&morse, next_mark_start));

    /* Walk the configured message by engine deadlines until its terminal
     * black pause. This verifies pause timing without binding the fixture to
     * any particular message text or length. */
    katmed_morse_start(&morse, 0);
    uint64_t now_ms = 0;
    size_t steps = 0;
    const size_t max_steps = strlen(message) * 16u + 16u;
    while (morse.active && morse.phase != 2u) {
        assert(++steps <= max_steps);
        assert(morse.due_ms > now_ms);
        now_ms = morse.due_ms;
        (void)katmed_morse_update(&morse, now_ms);
    }
    assert(morse.active && morse.phase == 2u && !morse.white);
    const uint64_t pause_end = morse.due_ms;
    const uint64_t pause_at = now_ms + 1000u;
    katmed_morse_pause(&morse, pause_at);
    assert(morse.active && morse.paused && !morse.white);
    const uint64_t resume_at = pause_at + 5000u;
    katmed_morse_resume(&morse, resume_at);
    const uint64_t resumed_pause_end = pause_end + (resume_at - pause_at);
    assert(!morse.paused && morse.due_ms == resumed_pause_end);
    assert(!katmed_morse_update(&morse, resumed_pause_end - 1u));
    assert(katmed_morse_update(&morse, resumed_pause_end));
    katmed_morse_stop(&morse);
    assert(!katmed_morse_update(&morse, resumed_pause_end + 1u));
    return 0;
}
