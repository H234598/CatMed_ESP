#include "katmed_morse.h"

#include <string.h>

#define MORSE_UNIT_MS 180u
#define MORSE_PAUSE_MS 45000u

/* User-supplied output data. Its contents are never interpreted as commands. */
static const char message[] =
    "Please customize the Morse text in your private profile.";

/* ITU-R M.1677-1 Annex 1 for A-Z, 0-9, . , ? / and the 1/3/7 gaps.
 * The common six-element ! (-.-.--) is an explicit extension. Umlauts are
 * transliterated AE/OE/UE, never discarded; ß would become SS. */
static const char *const letters[] = {
    ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---",
    "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-",
    "..-", "...-", ".--", "-..-", "-.--", "--.."};
static const char *const digits[] = {
    "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...",
    "---..", "----."};

const char *katmed_morse_message(void) { return message; }

static const char *pattern_for(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return letters[c - 'A'];
    if (c >= '0' && c <= '9') return digits[c - '0'];
    switch (c) {
    case '.': return ".-.-.-";
    case ',': return "--..--";
    case '?': return "..--..";
    case '/': return "-..-.";
    case '!': return "-.-.--";
    default: return NULL;
    }
}

/* Read exactly one normalized ASCII character, preserving word spaces. */
static char next_char(KatmedMorse *morse)
{
    if (morse->expansion) {
        char c = morse->expansion;
        morse->expansion = 0;
        return c;
    }
    const unsigned char *p = (const unsigned char *)message;
    unsigned char c = p[morse->byte_index];
    if (!c) return 0;
    ++morse->byte_index;
    if (c != 0xc3u) return (char)c;
    unsigned char second = p[morse->byte_index];
    if (!second) return 0;
    ++morse->byte_index;
    switch (second) {
    case 0xa4: case 0x84: morse->expansion = 'E'; return 'A';
    case 0xb6: case 0x96: morse->expansion = 'E'; return 'O';
    case 0xbc: case 0x9c: morse->expansion = 'E'; return 'U';
    case 0x9f: morse->expansion = 'S'; return 'S';
    default: return (char)0xff;
    }
}

bool katmed_morse_message_valid(void)
{
    KatmedMorse cursor = {0};
    char c;
    while ((c = next_char(&cursor)) != 0)
        if (c != ' ' && !pattern_for(c)) return false;
    return cursor.expansion == 0 && cursor.byte_index == strlen(message);
}

/* Called at a completed character. The outgoing gap is 3 units, or 7
 * whenever one or more word separators follow it. */
static bool next_pattern(KatmedMorse *morse, uint8_t *gap_units)
{
    bool word = false;
    char c;
    while ((c = next_char(morse)) != 0) {
        if (c == ' ') {
            word = true;
            continue;
        }
        morse->pattern = pattern_for(c);
        if (!morse->pattern) return false;
        morse->symbol_index = 0;
        *gap_units = word ? 7u : 3u;
        return true;
    }
    return false;
}

void katmed_morse_start(KatmedMorse *morse, uint64_t now_ms)
{
    if (!morse) return;
    *morse = (KatmedMorse){0};
    if (!katmed_morse_message_valid()) return;
    uint8_t ignored_gap;
    if (!next_pattern(morse, &ignored_gap)) return;
    morse->active = true;
    morse->white = true;
    morse->due_ms = now_ms + (morse->pattern[0] == '-' ? 3u : 1u) * MORSE_UNIT_MS;
}

void katmed_morse_stop(KatmedMorse *morse)
{
    if (morse) {
        morse->active = false;
        morse->paused = false;
        morse->remaining_ms = 0;
    }
}

void katmed_morse_pause(KatmedMorse *morse, uint64_t now_ms)
{
    if (!morse || !morse->active || morse->paused) return;
    (void)katmed_morse_update(morse, now_ms);
    if (!morse->active) return;
    morse->remaining_ms = morse->due_ms > now_ms ? morse->due_ms - now_ms : 0;
    morse->paused = true;
}

void katmed_morse_resume(KatmedMorse *morse, uint64_t now_ms)
{
    if (!morse || !morse->active || !morse->paused) return;
    morse->due_ms = now_ms + morse->remaining_ms;
    morse->remaining_ms = 0;
    morse->paused = false;
}

bool katmed_morse_update(KatmedMorse *morse, uint64_t now_ms)
{
    if (!morse || !morse->active) return false;
    if (morse->paused) return morse->white;
    while (now_ms >= morse->due_ms) {
        if (morse->phase == 2u) { /* 45-second black pause ends. */
            morse->byte_index = 0;
            morse->expansion = 0;
            uint8_t ignored_gap;
            if (!next_pattern(morse, &ignored_gap)) { morse->active = false; return false; }
            morse->phase = 0;
            morse->white = true;
            morse->due_ms += (morse->pattern[0] == '-' ? 3u : 1u) * MORSE_UNIT_MS;
        } else if (morse->phase == 1u) { /* A gap ends. */
            morse->phase = 0;
            morse->white = true;
            morse->due_ms += (morse->pattern[morse->symbol_index] == '-' ? 3u : 1u) * MORSE_UNIT_MS;
        } else { /* A white symbol ends. */
            morse->white = false;
            if (morse->pattern[morse->symbol_index + 1u]) {
                ++morse->symbol_index;
                morse->phase = 1;
                morse->due_ms += MORSE_UNIT_MS;
            } else {
                uint8_t gap_units;
                if (next_pattern(morse, &gap_units)) {
                    morse->phase = 1;
                    morse->due_ms += (uint64_t)gap_units * MORSE_UNIT_MS;
                } else {
                    morse->phase = 2;
                    morse->due_ms += MORSE_PAUSE_MS;
                }
            }
        }
    }
    return morse->white;
}
