#ifndef KATMED_TIME_IDF_H
#define KATMED_TIME_IDF_H

#include <stddef.h>
#include "esp_err.h"
#include "katmed_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call on root/IP role changes. Only the root with a configured server and
 * a real IP connection may start SNTP. Demotion or IP loss stops it. */
esp_err_t katmed_time_idf_set_root(KatmedTime *time, bool is_root, bool has_ip,
                                   const uint8_t *ntp_server, size_t length);

/* IPv4 (network byte order) observed at the last local SNTP callback while
 * this client is running; zero before sync, after stop, or if unsupported.
 * Mesh time reception does not supply a local SNTP server address. */
uint32_t katmed_time_idf_sync_server_ipv4(void);

/* Nonblocking. Returns one newly accepted local SNTP observation, to be
 * submitted to EVENT_TIME_SYNC before publishing as group time. */
bool katmed_time_idf_poll(KatmedTime *time, ClockSample *new_sync);

#ifdef __cplusplus
}
#endif
#endif
