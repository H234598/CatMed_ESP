#include "katmed_import.h"
#include "katmed_store.h"
#include "katmed_mesh_status.h"

#include <cstdlib>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_rom_crc.h"
#else
#include <zlib.h>
#endif

namespace {
constexpr size_t kMaxCapsule = 0x40000;
constexpr size_t kMaxFiles = 128;
constexpr size_t kMaxFile = 128 * 1024;
/* 64 KiB is reserved for LittleFS metadata, directory entries and writes.
 * This is a conservative preflight bound, never a capacity guarantee. */
constexpr size_t kMaxFileContent = 0x40000 - 65536;
constexpr char kImportNs[] = "katmed_imp";
constexpr char kStage[] = "stage";
constexpr char kJournal[] = "journal";
constexpr char kLiveNs[] = "katmed";
constexpr char kLiveKey[] = "state";
constexpr char kHistoryKey[] = "mesh_history";
constexpr size_t kStageMax = 4 + KATMED_STORE_BLOB_MAX + MESH_HISTORY_BLOB;

uint16_t le16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
        uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t le64(const uint8_t *p) {
    uint64_t n = 0;
    for (unsigned i = 0; i < 8; ++i) n |= uint64_t(p[i]) << (8 * i);
    return n;
}
void put16(uint8_t *p, uint16_t n) { p[0] = uint8_t(n); p[1] = uint8_t(n >> 8); }
void put32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(n >> (8 * i));
}
uint32_t crc(uint32_t previous, const uint8_t *p, size_t n) {
#ifdef ESP_PLATFORM
    return esp_rom_crc32_le(previous, p, n);
#else
    return uint32_t(crc32(previous, p, n));
#endif
}
void wipe(void *p, size_t n) {
    volatile uint8_t *q = static_cast<volatile uint8_t *>(p);
    while (n--) *q++ = 0;
}
bool callbacks(const KatmedImportBackend *b) {
    return b && b->read && b->write && b->erase &&
        b->target_empty && b->free_entries;
}
bool input_read(const KatmedImportInput *in, size_t at, uint8_t *p, size_t n) {
    return in && in->read && at <= in->length && n <= in->length - at &&
        in->read(in->context, at, p, n);
}
bool hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
bool image_name(const char *p) {
    if (std::strncmp(p, "littlefs/user-images/", 21) != 0) return false;
    const char *n = p + 21;
    if (std::strlen(n) == 16 && std::strncmp(n, "img-", 4) == 0 &&
        std::strcmp(n + 12, ".jpg") == 0) {
        for (unsigned i = 4; i < 12; ++i) if (!hex(n[i])) return false;
        return true;
    }
    if (std::strlen(n) == 12 && std::strncmp(n, "cat-", 4) == 0 &&
        n[6] == '-' && (n[7] == '0' || n[7] == '1') &&
        std::strcmp(n + 8, ".jpg") == 0) {
        return hex(n[4]) && hex(n[5]);
    }
    return false;
}
bool known_path(const char *p, bool *file) {
    static const char *const nvs[] = {
        "nvs/katmed/state", "nvs/katmed/mesh_history",
        "nvs/katmed_web/auth", "nvs/katmed_web/identity",
        "nvs/katmed_reset/journal"
    };
    static const char *const acme[] = {
        "account.json", "directory.json", "account.pem", "order.json",
        "certkey.pem", "certificate.pem"
    };
    *file = false;
    for (const char *name : nvs) if (std::strcmp(p, name) == 0) return true;
    if (image_name(p)) { *file = true; return true; }
    constexpr char prefix[] = "littlefs/acme/";
    if (std::strncmp(p, prefix, sizeof(prefix) - 1) == 0)
        for (const char *name : acme)
            if (std::strcmp(p + sizeof(prefix) - 1, name) == 0) {
                *file = true; return true;
            }
    return false; /* renewal.json and any future class fail closed. */
}
bool read_blob(const KatmedImportBackend *b, const char *ns, const char *key,
               uint8_t *out, size_t capacity, size_t *length) {
    size_t n = capacity;
    if (b->read(b->context, ns, key, out, &n) != KATMED_IMPORT_READ_OK ||
        n > capacity) return false;
    *length = n;
    return true;
}
bool absent(const KatmedImportBackend *b, const char *ns, const char *key) {
    size_t n = 0;
    return b->read(b->context, ns, key, nullptr, &n) ==
        KATMED_IMPORT_READ_ABSENT;
}
void make_journal(uint8_t out[16], size_t n, uint32_t digest) {
    std::memset(out, 0, 16);
    std::memcpy(out, "KMIJ", 4);
    out[4] = 1;
    put16(out + 6, uint16_t(n));
    put32(out + 8, digest);
    put32(out + 12, crc(0, out, 12));
}
bool valid_journal(const uint8_t *j, size_t n) {
    return n == 16 && std::memcmp(j, "KMIJ", 4) == 0 &&
        j[4] == 1 && j[5] == 0 && le16(j + 6) >= 12 &&
        le16(j + 6) <= kStageMax &&
        le32(j + 12) == crc(0, j, 12);
}
bool same_blob(const KatmedImportBackend *b, const char *ns, const char *key,
               const uint8_t *expected, size_t n, uint8_t *scratch,
               size_t capacity) {
    size_t got = 0;
    return read_blob(b, ns, key, scratch, capacity, &got) &&
        got == n && std::memcmp(scratch, expected, n) == 0;
}
bool valid_stage(const uint8_t *stage, size_t n, uint64_t local_device) {
    if (n < 16) return false;
    const size_t state_len = le16(stage);
    const size_t history_len = le16(stage + 2);
    if (state_len < 20 || state_len > KATMED_STORE_BLOB_MAX ||
        (history_len != 0 && history_len != MESH_HISTORY_BLOB &&
         history_len != MESH_HISTORY_LEGACY_BLOB) ||
        n != 4 + state_len + history_len) return false;
    uint8_t group[16];
    /* The canonical v11 Store header carries its device ID at byte 12. */
    if (!local_device ||
        !store_import_blob_group(stage + 4, state_len, group) ||
        le64(stage + 4 + 12) != local_device) return false;
    if (!history_len) return true;
    MeshHistory *history = static_cast<MeshHistory *>(std::calloc(1, sizeof(MeshHistory)));
    if (!history) return false;
    bool ok = mesh_history_decode(history, stage + 4 + state_len,
                                  history_len, group) == STORE_OK;
    wipe(history, sizeof(*history));
    std::free(history);
    return ok;
}
bool live_matches(const KatmedImportBackend *b, const uint8_t *stage,
                  uint8_t *scratch) {
    const size_t state_len = le16(stage), history_len = le16(stage + 2);
    return same_blob(b, kLiveNs, kLiveKey, stage + 4, state_len, scratch,
                     kStageMax) &&
        (!history_len ||
         same_blob(b, kLiveNs, kHistoryKey, stage + 4 + state_len,
                   history_len, scratch, kStageMax));
}
bool commit_live(const KatmedImportBackend *b, const uint8_t *stage,
                 uint8_t *scratch) {
    const size_t state_len = le16(stage), history_len = le16(stage + 2);
    if (!b->write(b->context, kLiveNs, kLiveKey, stage + 4, state_len) ||
        !same_blob(b, kLiveNs, kLiveKey, stage + 4, state_len, scratch,
                   kStageMax)) return false;
    if (history_len &&
        (!b->write(b->context, kLiveNs, kHistoryKey, stage + 4 + state_len,
                   history_len) ||
         !same_blob(b, kLiveNs, kHistoryKey, stage + 4 + state_len,
                    history_len, scratch, kStageMax))) return false;
    return true;
}
}

extern "C" bool katmed_import_resume(const KatmedImportBackend *b,
                                       uint64_t local_device) {
    if (!callbacks(b) || !local_device) return false;
    uint8_t journal[16];
    size_t jlen = sizeof(journal);
    KatmedImportRead jr = b->read(b->context, kImportNs, kJournal,
                                  journal, &jlen);
    if (jr == KATMED_IMPORT_READ_ERROR) return false;
    if (jr == KATMED_IMPORT_READ_ABSENT &&
        absent(b, kImportNs, kStage)) return true;
    uint8_t *stage = static_cast<uint8_t *>(std::malloc(kStageMax));
    uint8_t *check = static_cast<uint8_t *>(std::malloc(kStageMax));
    if (!stage || !check) { std::free(stage); std::free(check); return false; }
    size_t n = 0;
    bool ok = false;
    if (jr == KATMED_IMPORT_READ_ABSENT) {
        /* A crash before journal publication leaves an orphan. Hold boot.
         * A crash after journal removal may leave a verified live copy. */
        if (absent(b, kImportNs, kStage)) ok = true;
        else if (read_blob(b, kImportNs, kStage, stage, kStageMax, &n) &&
                 valid_stage(stage, n, local_device) &&
                 live_matches(b, stage, check))
            ok = b->erase(b->context, kImportNs, kStage);
    } else if (valid_journal(journal, jlen) &&
               read_blob(b, kImportNs, kStage, stage, kStageMax, &n) &&
               n == le16(journal + 6) &&
               crc(0, stage, n) == le32(journal + 8) &&
               valid_stage(stage, n, local_device) &&
               commit_live(b, stage, check) &&
               b->erase(b->context, kImportNs, kJournal)) {
        /* A failed final cleanup is recoverable on the next boot. */
        ok = b->erase(b->context, kImportNs, kStage);
    }
    wipe(stage, kStageMax);
    wipe(check, kStageMax);
    std::free(stage); std::free(check);
    return ok;
}

extern "C" bool katmed_import_abort(const KatmedImportBackend *b) {
    return callbacks(b) && absent(b, kImportNs, kJournal) &&
        absent(b, kLiveNs, kLiveKey) &&
        absent(b, kLiveNs, kHistoryKey) &&
        b->erase(b->context, kImportNs, kStage);
}

extern "C" bool katmed_import_apply(const KatmedImportInput *in,
                                     const KatmedImportBackend *b) {
    if (!callbacks(b) || !in || !in->read || !in->expected_device ||
        in->length < 16 ||
        in->length > kMaxCapsule) return false;
    uint8_t header[16];
    if (!input_read(in, 0, header, sizeof(header)) ||
        std::memcmp(header, "KMI1", 4) != 0 ||
        le32(header + 4) != in->length ||
        le16(header + 8) == 0 || le16(header + 8) > kMaxFiles ||
        le16(header + 10) != 0) return false;
    char (*names)[64] = static_cast<char (*)[64]>(
        std::calloc(le16(header + 8), 64));
    uint8_t *state = static_cast<uint8_t *>(std::malloc(KATMED_STORE_BLOB_MAX));
    uint8_t *history = static_cast<uint8_t *>(std::malloc(MESH_HISTORY_BLOB));
    if (!names || !state || !history) {
        std::free(names); std::free(state); std::free(history); return false;
    }
    size_t at = 16, state_len = 0, history_len = 0, file_bytes = 0, nvs_bytes = 0;
    uint32_t overall = 0;
    bool supported = true, ok = true, have_state = false;
    uint8_t chunk[512];
    for (unsigned i = 0; ok && i < le16(header + 8); ++i) {
        uint8_t record[12];
        if (!input_read(in, at, record, sizeof(record))) { ok = false; break; }
        overall = crc(overall, record, sizeof(record));
        at += sizeof(record);
        size_t path_len = le16(record), length = le32(record + 4);
        if (le16(record + 2) || path_len == 0 || path_len >= 64 ||
            length > kMaxFile || path_len > in->length - at) {
            ok = false; break;
        }
        if (!input_read(in, at, reinterpret_cast<uint8_t *>(names[i]),
                        path_len)) { ok = false; break; }
        if (std::strlen(names[i]) != path_len) { ok = false; break; }
        overall = crc(overall, reinterpret_cast<const uint8_t *>(names[i]), path_len);
        at += path_len;
        bool file = false;
        if (!known_path(names[i], &file) || length > in->length - at) {
            ok = false; break;
        }
        for (unsigned j = 0; j < i; ++j)
            if (std::strcmp(names[j], names[i]) == 0) ok = false;
        if (!ok) break;
        if (file) {
            if (length > kMaxFileContent - file_bytes) { ok = false; break; }
            file_bytes += length;
        } else {
            if (length > 4096 || length > 16384 - nvs_bytes) {
                ok = false; break;
            }
            nvs_bytes += length;
        }
        const bool this_state = std::strcmp(names[i], "nvs/katmed/state") == 0;
        const bool this_history = std::strcmp(names[i], "nvs/katmed/mesh_history") == 0;
        if (this_state) {
            have_state = true;
            state_len = length;
        } else if (this_history) {
            history_len = length;
            /* A declared empty record is not an absent history. Both accepted
             * sizes still pass the mesh codec in valid_stage() below. */
            if (length != MESH_HISTORY_BLOB &&
                length != MESH_HISTORY_LEGACY_BLOB) { ok = false; break; }
        } else supported = false;
        uint32_t item = 0;
        for (size_t off = 0; off < length && ok;) {
            size_t n = length - off < sizeof(chunk) ? length - off : sizeof(chunk);
            if (!input_read(in, at + off, chunk, n)) { ok = false; break; }
            overall = crc(overall, chunk, n);
            item = crc(item, chunk, n);
            if (this_state) std::memcpy(state + off, chunk, n);
            if (this_history) std::memcpy(history + off, chunk, n);
            off += n;
        }
        if (item != le32(record + 8)) ok = false;
        at += length;
    }
    ok = ok && at == in->length && overall == le32(header + 12) &&
        have_state && supported;
    uint8_t *stage = static_cast<uint8_t *>(std::malloc(kStageMax));
    if (ok) {
        ok = stage != nullptr;
        if (ok) {
            put16(stage, uint16_t(state_len));
            put16(stage + 2, uint16_t(history_len));
            std::memcpy(stage + 4, state, state_len);
            if (history_len) std::memcpy(stage + 4 + state_len, history, history_len);
            ok = valid_stage(stage, 4 + state_len + history_len,
                             in->expected_device);
        }
    }
    if (ok) {
        size_t free = 0;
        const size_t stage_len = 4 + state_len + history_len;
        const size_t entries = (stage_len + 31) / 32 + 4 +
            (state_len + 31) / 32 + 4 +
            (history_len ? (history_len + 31) / 32 + 4 : 0);
        ok = b->target_empty(b->context) &&
            absent(b, kImportNs, kStage) &&
            absent(b, kImportNs, kJournal) &&
            b->free_entries(b->context, &free) &&
            free >= entries + 24;
    }
    if (ok) {
        uint8_t journal[16];
        const size_t stage_len = 4 + state_len + history_len;
        make_journal(journal, stage_len, crc(0, stage, stage_len));
        uint8_t *check = static_cast<uint8_t *>(std::malloc(kStageMax));
        ok = check &&
            b->write(b->context, kImportNs, kStage, stage, stage_len) &&
            same_blob(b, kImportNs, kStage, stage, stage_len, check, kStageMax) &&
            b->write(b->context, kImportNs, kJournal, journal, sizeof(journal)) &&
            katmed_import_resume(b, in->expected_device);
        if (check) { wipe(check, kStageMax); std::free(check); }
        /* Before the journal is committed an orphan may be removed. Once the
         * journal exists only replay may touch the live key. */
        if (!ok && absent(b, kImportNs, kJournal))
            (void)katmed_import_abort(b);
    }
    wipe(state, KATMED_STORE_BLOB_MAX);
    wipe(history, MESH_HISTORY_BLOB);
    wipe(chunk, sizeof(chunk));
    if (stage) wipe(stage, kStageMax);
    std::free(state); std::free(history); std::free(stage); std::free(names);
    return ok;
}
