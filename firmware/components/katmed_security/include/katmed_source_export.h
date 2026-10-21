#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Source side of tools/security_migration_contract.md. Production calls run
 * only through the boot snapshot window, after NVS and user_images are ready.
 * The source reader neither initializes storage nor mounts, writes, erases,
 * or logs any private data. */
typedef struct {
    void *context;
    bool (*begin)(void *context, const char *logical_path, size_t length);
    bool (*bytes)(void *context, const uint8_t *data, size_t length);
    bool (*end)(void *context);
    /* With a non-NULL sink and finish callback, called exactly once, including
     * when begin/bytes/end is missing. On false, discard all received bytes.
     * On true, receiver may create source-inventory.json from begin paths. */
    void (*finish)(void *context, bool success);
    /* Cooperative cancellation; the boot snapshot requires this callback.
     * All callbacks must return promptly. */
    bool (*keep_going)(void *context);
} KatmedSourceExportSink;

/* Boot-only export boundary. The application opens this window only after
 * import and reset replay and the user-image mount, and closes it before the
 * Store, Mesh, Web or ACME services are started. A future transport supplies
 * a bounded sink through katmed_source_snapshot_boot_sink(). */
void katmed_source_snapshot_boot_window_open(void);
void katmed_source_snapshot_boot_window_close(void);
bool katmed_source_snapshot_boot(const KatmedSourceExportSink *sink,
                                 bool (*uncontrolled_writers_idle)(void));
/* Weak default is NULL. A later bounded transport may override this hook. */
const KatmedSourceExportSink *katmed_source_snapshot_boot_sink(void);

/* A writer holds a lease from before its first mutation until its last
 * publish/cleanup. Entry is nonblocking and is refused during export.
 * Leave must run once per successful enter on the same task. A foreign or
 * extra leave cannot release another task's lease. */
bool katmed_source_writer_enter(void);
void katmed_source_writer_leave(void);

/* NULL sink or missing finish is an invalid, non-callback-capable precondition:
 * returns false without a callback. Other incomplete sinks finish(false). */
bool katmed_source_export(const KatmedSourceExportSink *sink);

#ifdef __cplusplus
}
#endif
