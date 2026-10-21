#ifndef KATMED_ACME_H
#define KATMED_ACME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* No URL, email, CA, EAB material or filesystem prefix is compiled in. A
 * caller must obtain all values from a reviewed secure configuration owner. */
typedef struct {
    const char *identifier;
    const char *email;
    const char *server;
    const char *state_prefix;
} KatmedAcmeConfig;

/* NULL is the normal Katmed build path: the SDK is linked, but no ACME
 * network or state mutation is enabled. */
bool katmed_acme_start(const KatmedAcmeConfig *config);
void katmed_acme_poll(uint64_t now_ms);
void katmed_acme_stop(void);
bool katmed_acme_certificate_changed(void);
bool katmed_acme_quiescent(void);

/* Built only into the paired source experiment. Normal firmware never calls
 * this entry point and never starts an ACME exchange during boot. */
bool katmed_acme_resource_probe_launch(void);

#ifdef __cplusplus
}
#endif
#endif
