#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The caller supplies a stable, complete firmware-import-v1.bin from the
 * protected external import-stage. Secure NVS must already be initialized,
 * and the caller must stop all normal storage writers.
 * A CRC detects accidental transport damage, not a malicious replacement.
 * No caller or transport is wired into the running application yet. */
typedef struct {
    void *context;
    size_t length;
    /* Read-only local boot identity; must match the v11 Store device. */
    uint64_t expected_device;
    bool (*read)(void *context, size_t offset, uint8_t *out, size_t length);
} KatmedImportInput;

typedef enum {
    KATMED_IMPORT_READ_OK,
    KATMED_IMPORT_READ_ABSENT,
    KATMED_IMPORT_READ_ERROR
} KatmedImportRead;

/* Every write and erase callback must commit durably before returning true. */
typedef struct {
    void *context;
    /* out==NULL probes presence; ABSENT and ERROR must be distinct. */
    KatmedImportRead (*read)(void *, const char *ns, const char *key,
                             uint8_t *, size_t *);
    bool (*write)(void *, const char *ns, const char *key,
                  const uint8_t *, size_t);
    bool (*erase)(void *, const char *ns, const char *key);
    /* Enumerate the entire target NVS, including unknown SDK/application keys. */
    bool (*target_empty)(void *);
    bool (*free_entries)(void *, size_t *);
} KatmedImportBackend;

/* Currently accepts only canonical katmed/state and optional mesh_history.
 * Other known classes are scanned but fail closed before target mutation. */
bool katmed_import_apply(const KatmedImportInput *input,
                         const KatmedImportBackend *backend);
/* Called before Store/Web/mesh startup with the locally read device ID.
 * A pending journal or cleanup stage is codec/CRC checked and bound to that
 * ID before any live write or stage/journal erase. Mismatch holds startup. */
bool katmed_import_resume(const KatmedImportBackend *backend,
                          uint64_t local_device);
/* Only an uncommitted orphan stage may be explicitly aborted. */
bool katmed_import_abort(const KatmedImportBackend *backend);

#ifdef ESP_PLATFORM
bool katmed_import_apply_idf(const KatmedImportInput *input);
bool katmed_import_resume_idf(void);
bool katmed_import_abort_idf(void);
#endif

#ifdef __cplusplus
}
#endif
