#ifndef KATMED_INTERACTION_H
#define KATMED_INTERACTION_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define KATMED_INTERACTION_IMPULSES 4u
/* All calls are serialized on the application loop. No persistent medical event. */
typedef enum {
    KATMED_INTERACTION_ENTER = 1,
    KATMED_INTERACTION_LEAVE,
    KATMED_INTERACTION_DOWN,
    KATMED_INTERACTION_UP,
    KATMED_INTERACTION_CANCEL,
    KATMED_INTERACTION_FINISH,
    KATMED_INTERACTION_CONTINUE,
    KATMED_INTERACTION_ACK_FINISHED,
    KATMED_INTERACTION_DISMISS_GREETING,
    KATMED_INTERACTION_ALARM,
    KATMED_INTERACTION_CONSUME_IMPULSE
} KatmedInteractionInputKind;
typedef struct {
    KatmedInteractionInputKind kind;
    uint64_t menu_entry; /* nonzero, new on each ENTER; not needed for reception */
    uint64_t contact_id; /* nonzero on DOWN/UP; never reused within an entry */
    uint64_t grant_id;   /* zero allowed for first DOWN while requesting */
    uint64_t impulse_id; /* CONSUME_IMPULSE; contact_id identifies local edges */
    uint64_t target_id;  /* prompt_id / completion_id / greeting_id for exact ACK */
    uint64_t contact_time_ms; /* original local DOWN/UP capture time, never FSM time */
    bool contact_time_valid; /* false: use current delivery time; true: <= delivery and ordered */
    bool active;        /* ALARM: true starts, false ends; always independent of menu */
} KatmedInteractionInput;
typedef struct {
    uint64_t sender, grant_id, impulse_id, state_revision;
    uint64_t valid_until_ms; /* local monotonic absolute deadline */
    uint32_t duration_ms;
} KatmedInteractionImpulse;
typedef enum {
    KATMED_INTERACTION_OFFLINE = 0,
    KATMED_INTERACTION_SYNCING,
    KATMED_INTERACTION_READY,
    KATMED_INTERACTION_REQUESTING,
    KATMED_INTERACTION_OWNER,
    KATMED_INTERACTION_REMOTE,
    KATMED_INTERACTION_FINISHED
} KatmedInteractionMode;
typedef struct {
    bool connected, synchronized, menu_open, alarm_active;
    bool can_request, owns_token, key_down, receive_white;
    bool finish_prompt, sender_finished, greeting_visible, overflow;
    KatmedInteractionMode mode;
    uint64_t authority_id, authority_generation;
    uint64_t menu_entry, sender_id, grant_id, impulse_id, state_revision;
    uint64_t receive_until_ms, prompt_id, completion_id;
    uint64_t greeting_id, greeting_until_ms;
    uint16_t known_devices;
    uint8_t impulse_count;
    /* Ordered completed impulses not already displayed live. Consume exactly
     * once with grant_id+impulse_id, even if discarded by alarm/UI deadline. */
    KatmedInteractionImpulse impulses[KATMED_INTERACTION_IMPULSES];
} KatmedInteractionStatus;
#ifdef __cplusplus
}
#endif
#endif
