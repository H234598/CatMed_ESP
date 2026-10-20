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

/* A file name is encoded from kind/id; no arbitrary target path is accepted.
 * kind 1 = uploaded img-ID, kind 2 = catalogue index/profile packed as
 * (index << 1) | profile. input_offset is used only while staging. */
typedef struct {
    uint8_t kind;
    uint32_t id;
    uint32_t length;
    uint32_t crc;
    size_t input_offset;
} KatmedImportFile;

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
    /* IDF implementation uses the product Web X.509/key-pair validator. */
    bool (*validate_web)(void *, const uint8_t *auth, size_t auth_len,
                         const uint8_t *identity, size_t identity_len);
    /* File callbacks operate on the real mounted user_images LittleFS. The
     * IDF implementation never formats it. Each success is durable and
     * checked against the exact name, length and CRC in the NVS stage. */
    bool (*files_empty)(void *, size_t content_bytes, size_t count);
    bool (*file_stage)(void *, unsigned ordinal, const KatmedImportFile *,
                       const KatmedImportInput *);
    bool (*file_publish)(void *, unsigned ordinal, const KatmedImportFile *);
    bool (*file_final)(void *, unsigned ordinal, const KatmedImportFile *);
    bool (*file_abort)(void *, unsigned ordinal, const KatmedImportFile *);
} KatmedImportBackend;

/* Accepts canonical v11 NVS and bounded user-images; ACME fails closed. */
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
