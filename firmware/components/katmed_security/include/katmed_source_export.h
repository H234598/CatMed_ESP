#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Source side of tools/security_migration_contract.md. The caller must stop
 * writers and have mounted NVS and user_images already. This function neither
 * initializes storage nor mounts, writes, erases, or logs anything. No caller
 * is wired into the running firmware yet. */
typedef struct {
    void *context;
    bool (*begin)(void *context, const char *logical_path, size_t length);
    bool (*bytes)(void *context, const uint8_t *data, size_t length);
    bool (*end)(void *context);
    /* With a non-NULL sink and finish callback, called exactly once, including
     * when begin/bytes/end is missing. On false, discard all received bytes.
     * On true, receiver may create source-inventory.json from begin paths. */
    void (*finish)(void *context, bool success);
} KatmedSourceExportSink;

/* NULL sink or missing finish is an invalid, non-callback-capable precondition:
 * returns false without a callback. Other incomplete sinks finish(false). */
bool katmed_source_export(const KatmedSourceExportSink *sink);

#ifdef __cplusplus
}
#endif
