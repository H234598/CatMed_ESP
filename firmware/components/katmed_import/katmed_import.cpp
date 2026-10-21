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
constexpr size_t kMaxCapsule = 0x50000;
constexpr size_t kMaxFiles = 128;
constexpr size_t kMaxFile = 128 * 1024;
/* The source contract permits 248 KiB; the expanded 576 KiB security target
 * still reserves space for LittleFS metadata and copy-on-write operations. */
constexpr size_t kMaxFileContent = 0x40000 - 8192;
constexpr char kImportNs[] = "katmed_imp";
constexpr char kStage[] = "stage";
constexpr char kJournal[] = "journal";
constexpr char kLiveNs[] = "katmed";
constexpr char kLiveKey[] = "state";
constexpr char kHistoryKey[] = "mesh_history";
constexpr size_t kAuthSize = 1112;
constexpr size_t kIdentitySize = 1288;
constexpr size_t kResetSize = 2;
constexpr size_t kStageHeader = 14;
constexpr size_t kFileHeader = 16;
constexpr size_t kFileDescriptor = 13;
constexpr size_t kStageMax = kFileHeader + KATMED_STORE_BLOB_MAX +
    MESH_HISTORY_BLOB + kAuthSize + kIdentitySize + kResetSize +
    kMaxFiles * kFileDescriptor;
enum Part { State, History, Auth, Identity, Reset, PartCount };
struct LiveKey { const char *ns; const char *key; };
constexpr LiveKey kLiveKeys[PartCount] = {
    {kLiveNs, kLiveKey}, {kLiveNs, kHistoryKey},
    {"katmed_web", "auth"}, {"katmed_web", "identity"},
    {"katmed_reset", "journal"}
};
struct StageView {
    size_t length[PartCount]{};
    const uint8_t *data[PartCount]{};
    const uint8_t *files = nullptr;
    size_t file_count = 0;
};

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
bool input_read(const KatmedImportInput *in, size_t at, uint8_t *p, size_t n);
bool image_name(const char *p);
KatmedImportFile file_at(const StageView &view, size_t i) {
    const uint8_t *p = view.files + i * kFileDescriptor;
    return {p[0], le32(p + 1), le32(p + 5), le32(p + 9), 0};
}
bool valid_file(const KatmedImportFile &f) {
    return f.length >= 16 && f.length <= kMaxFile &&
        ((f.kind == 1 && f.id > 0 && f.id < 0x80000000u) ||
         (f.kind == 2 && (f.id >> 1) < 72));
}
bool same_file(const KatmedImportFile &a, const KatmedImportFile &b) {
    return a.kind == b.kind && a.id == b.id;
}
bool parse_image(const char *path, KatmedImportFile *f) {
    constexpr char prefix[] = "littlefs/user-images/";
    if (std::strncmp(path, prefix, sizeof(prefix) - 1) != 0) return false;
    const char *name = path + sizeof(prefix) - 1;
    if (!image_name(path)) return false;
    if (name[0] == 'i') {
        uint32_t id = 0;
        for (unsigned i = 4; i < 12; ++i) {
            const char c = name[i];
            id = id * 16 + uint32_t(c <= '9' ? c - '0' : c - 'a' + 10);
        }
        f->kind = 1; f->id = id;
    } else {
        const auto digit = [](char c) -> uint32_t {
            return uint32_t(c <= '9' ? c - '0' : c - 'a' + 10);
        };
        f->kind = 2;
        f->id = ((digit(name[4]) * 16 + digit(name[5])) << 1) |
            uint32_t(name[7] - '0');
    }
    f->length = 16;
    return valid_file(*f);
}
/* Baseline JPEG header/EOI validation follows the real upload parser's SOF0
 * bounds. A capsule with a truncated tail never reaches NVS staging. */
bool valid_jpeg(const KatmedImportInput *in, size_t offset, size_t length) {
    if (length < 16 || length > kMaxFile) return false;
    uint8_t pair[2];
    if (!input_read(in, offset, pair, 2) || pair[0] != 0xff || pair[1] != 0xd8 ||
        !input_read(in, offset + length - 2, pair, 2) ||
        pair[0] != 0xff || pair[1] != 0xd9) return false;
    size_t at = 2;
    while (at + 4 <= length - 2) {
        uint8_t prefix = 0, marker = 0;
        if (!input_read(in, offset + at++, &prefix, 1) || prefix != 0xff) return false;
        do {
            if (!input_read(in, offset + at++, &marker, 1)) return false;
        } while (marker == 0xff && at < length - 2);
        if (marker == 0 || marker == 0xd8 || marker == 0xd9 ||
            (marker >= 0xd0 && marker <= 0xd7)) return false;
        uint8_t size[2];
        if (!input_read(in, offset + at, size, 2)) return false;
        const size_t segment = size_t(size[0]) * 256 + size[1];
        if (segment < 2 || segment > length - 2 - at) return false;
        if (marker == 0xc0) {
            if (segment < 8) return false;
            uint8_t frame[6];
            if (!input_read(in, offset + at + 2, frame, sizeof(frame))) return false;
            const unsigned height = unsigned(frame[1]) * 256 + frame[2];
            const unsigned width = unsigned(frame[3]) * 256 + frame[4];
            return frame[0] == 8 && (frame[5] == 1 || frame[5] == 3) &&
                width > 0 && width <= 800 && height > 0 && height <= 800;
        }
        if (marker == 0xc2 || marker == 0xda) return false;
        at += segment;
    }
    return false;
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
bool stage_view(const uint8_t *stage, size_t n, StageView *view) {
    if (!stage || !view || n < 16) return false;
    *view = StageView{};
    size_t header = 4;
    view->length[State] = le16(stage);
    view->length[History] = le16(stage + 2);
    const bool files = n >= kFileHeader && std::memcmp(stage, "KMS3", 4) == 0;
    if (files || (n >= kStageHeader && std::memcmp(stage, "KMS2", 4) == 0)) {
        header = files ? kFileHeader : kStageHeader;
        for (unsigned i = 0; i < PartCount; ++i)
            view->length[i] = le16(stage + 4 + 2 * i);
        if (files) view->file_count = le16(stage + 14);
    }
    if (view->length[State] < 20 ||
        view->length[State] > KATMED_STORE_BLOB_MAX ||
        (view->length[History] != 0 &&
         view->length[History] != MESH_HISTORY_BLOB &&
         view->length[History] != MESH_HISTORY_LEGACY_BLOB) ||
        (view->length[Auth] && view->length[Auth] != kAuthSize) ||
        (view->length[Identity] && view->length[Identity] != kIdentitySize) ||
        (view->length[Reset] && view->length[Reset] != kResetSize) ||
        (view->length[Auth] && !view->length[Identity]) ||
        view->file_count > kMaxFiles) return false;
    size_t at = header;
    for (unsigned i = 0; i < PartCount; ++i) {
        if (view->length[i] > n - at) return false;
        view->data[i] = stage + at;
        at += view->length[i];
    }
    if (view->file_count > (n - at) / kFileDescriptor ||
        at + view->file_count * kFileDescriptor != n) return false;
    view->files = stage + at;
    size_t content = 0;
    for (size_t i = 0; i < view->file_count; ++i) {
        const KatmedImportFile f = file_at(*view, i);
        if (!valid_file(f) || f.length > kMaxFileContent - content) return false;
        content += f.length;
        for (size_t j = 0; j < i; ++j)
            if (same_file(f, file_at(*view, j))) return false;
    }
    return true;
}
bool valid_stage(const uint8_t *stage, size_t n, uint64_t local_device,
                 const KatmedImportBackend *b) {
    StageView view{};
    if (!stage_view(stage, n, &view)) return false;
    uint8_t group[16];
    /* The canonical v11 Store header carries its device ID at byte 12. */
    if (!local_device ||
        !store_import_blob_group(view.data[State], view.length[State], group) ||
        le64(view.data[State] + 12) != local_device) return false;
    if (view.length[History]) {
        MeshHistory *history = static_cast<MeshHistory *>(std::calloc(1, sizeof(MeshHistory)));
        if (!history) return false;
        const bool good = mesh_history_decode(history, view.data[History],
                                               view.length[History], group) == STORE_OK;
        wipe(history, sizeof(*history));
        std::free(history);
        if (!good) return false;
    }
    if (view.length[Reset] &&
        (view.data[Reset][0] != 0 || view.data[Reset][1] > 5)) return false;
    if (view.length[Auth]) {
        const uint8_t *auth = view.data[Auth];
        const size_t cert_len = le16(auth + 6);
        if (le32(auth) != 0x4b574131u || le16(auth + 4) != 1 ||
            cert_len > 1024) return false;
        for (size_t i = 88 + cert_len; i < kAuthSize; ++i)
            if (auth[i] != 0) return false;
        if (!cert_len) for (size_t i = 56; i < 88; ++i)
            if (auth[i] != 0) return false;
    }
    if (view.length[Identity]) {
        const uint8_t *identity = view.data[Identity];
        const size_t key_len = le16(identity + 4);
        const size_t cert_len = le16(identity + 6);
        if (le32(identity) != 0x4b574931u || !key_len || key_len > 256 ||
            !cert_len || cert_len > 1024) return false;
        for (size_t i = 8 + key_len; i < 264; ++i)
            if (identity[i] != 0) return false;
        for (size_t i = 264 + cert_len; i < kIdentitySize; ++i)
            if (identity[i] != 0) return false;
    }
    if ((view.length[Auth] || view.length[Identity]) &&
        (!b || !b->validate_web ||
         !b->validate_web(b->context, view.data[Auth], view.length[Auth],
                          view.data[Identity], view.length[Identity]))) return false;
    if (view.file_count && (!b || !b->files_empty || !b->file_stage ||
        !b->file_publish || !b->file_final || !b->file_abort)) return false;
    return true;
}
bool files_final(const KatmedImportBackend *b, const uint8_t *stage, size_t n) {
    StageView view{};
    if (!stage_view(stage, n, &view)) return false;
    for (size_t i = 0; i < view.file_count; ++i) {
        const KatmedImportFile f = file_at(view, i);
        if (!b->file_final(b->context, unsigned(i), &f)) return false;
    }
    return true;
}
bool publish_files(const KatmedImportBackend *b, const uint8_t *stage, size_t n) {
    StageView view{};
    if (!stage_view(stage, n, &view)) return false;
    for (size_t i = 0; i < view.file_count; ++i) {
        const KatmedImportFile f = file_at(view, i);
        if (!b->file_publish(b->context, unsigned(i), &f)) return false;
    }
    return files_final(b, stage, n);
}
bool live_matches(const KatmedImportBackend *b, const uint8_t *stage,
                  size_t n, uint8_t *scratch) {
    StageView view{};
    if (!stage_view(stage, n, &view)) return false;
    for (unsigned i = 0; i < PartCount; ++i) {
        if (view.length[i] &&
            !same_blob(b, kLiveKeys[i].ns, kLiveKeys[i].key,
                       view.data[i], view.length[i], scratch, kStageMax)) return false;
    }
    return true;
}
bool commit_live(const KatmedImportBackend *b, const uint8_t *stage,
                 size_t n, uint8_t *scratch) {
    StageView view{};
    if (!stage_view(stage, n, &view)) return false;
    constexpr unsigned order[] = {State, History, Identity, Auth, Reset};
    for (unsigned i : order) {
        if (!view.length[i]) continue;
        size_t present_length = 0;
        const KatmedImportRead present = b->read(b->context,
            kLiveKeys[i].ns, kLiveKeys[i].key, nullptr, &present_length);
        if (present == KATMED_IMPORT_READ_ERROR) return false;
        if (present == KATMED_IMPORT_READ_ABSENT &&
            !b->write(b->context, kLiveKeys[i].ns, kLiveKeys[i].key,
                      view.data[i], view.length[i])) return false;
        /* A replay may encounter its own committed key. An unrelated or
         * damaged live value is never overwritten by a CRC-only journal. */
        if (!same_blob(b, kLiveKeys[i].ns, kLiveKeys[i].key,
                       view.data[i], view.length[i], scratch, kStageMax)) return false;
    }
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
                 valid_stage(stage, n, local_device, b) &&
                 live_matches(b, stage, n, check) &&
                 files_final(b, stage, n))
            ok = b->erase(b->context, kImportNs, kStage);
    } else if (valid_journal(journal, jlen) &&
               read_blob(b, kImportNs, kStage, stage, kStageMax, &n) &&
               n == le16(journal + 6) &&
               crc(0, stage, n) == le32(journal + 8) &&
               valid_stage(stage, n, local_device, b) &&
               publish_files(b, stage, n) &&
               commit_live(b, stage, n, check) &&
               b->erase(b->context, kImportNs, kJournal)) {
        /* A failed final cleanup is recoverable on the next boot. */
        ok = b->erase(b->context, kImportNs, kStage);
    }
    wipe(stage, kStageMax);
    wipe(check, kStageMax);
    std::free(stage); std::free(check);
    return ok;
}

extern "C" bool katmed_import_abort(const KatmedImportBackend *b,
                                      uint64_t local_device) {
    if (!callbacks(b) || !local_device ||
        !absent(b, kImportNs, kJournal)) return false;
    for (const LiveKey &key : kLiveKeys)
        if (!absent(b, key.ns, key.key)) return false;
    uint8_t *stage = static_cast<uint8_t *>(std::malloc(kStageMax));
    if (!stage) return false;
    size_t n = 0;
    bool ok = read_blob(b, kImportNs, kStage, stage, kStageMax, &n);
    StageView view{};
    if (ok) ok = valid_stage(stage, n, local_device, b);
    if (ok) ok = stage_view(stage, n, &view);
    if (ok && view.file_count) {
        if (!b->file_abort) ok = false;
        for (size_t i = 0; ok && i < view.file_count; ++i) {
            const KatmedImportFile f = file_at(view, i);
            ok = b->file_abort(b->context, unsigned(i), &f);
        }
    }
    wipe(stage, kStageMax);
    std::free(stage);
    if (!ok) return false;
    return b->erase(b->context, kImportNs, kStage);
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
    KatmedImportFile *files = static_cast<KatmedImportFile *>(
        std::calloc(le16(header + 8), sizeof(KatmedImportFile)));
    uint8_t *state = static_cast<uint8_t *>(std::malloc(KATMED_STORE_BLOB_MAX));
    uint8_t *history = static_cast<uint8_t *>(std::malloc(MESH_HISTORY_BLOB));
    uint8_t *auth = static_cast<uint8_t *>(std::malloc(kAuthSize));
    uint8_t *identity = static_cast<uint8_t *>(std::malloc(kIdentitySize));
    uint8_t reset[kResetSize]{};
    if (!names || !files || !state || !history || !auth || !identity) {
        std::free(names); std::free(files); std::free(state); std::free(history);
        std::free(auth); std::free(identity); return false;
    }
    size_t at = 16, state_len = 0, history_len = 0, auth_len = 0,
           identity_len = 0, reset_len = 0, file_bytes = 0, file_count = 0,
           nvs_bytes = 0;
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
            if (length < 16 || length > kMaxFileContent - file_bytes) {
                ok = false; break;
            }
            file_bytes += length;
        } else {
            if (length > 4096 || length > 16384 - nvs_bytes) {
                ok = false; break;
            }
            nvs_bytes += length;
        }
        const bool this_state = std::strcmp(names[i], "nvs/katmed/state") == 0;
        const bool this_history = std::strcmp(names[i], "nvs/katmed/mesh_history") == 0;
        const bool this_auth = std::strcmp(names[i], "nvs/katmed_web/auth") == 0;
        const bool this_identity = std::strcmp(names[i], "nvs/katmed_web/identity") == 0;
        const bool this_reset = std::strcmp(names[i], "nvs/katmed_reset/journal") == 0;
        if (file) {
            KatmedImportFile f{};
            if (!parse_image(names[i], &f) ||
                !valid_jpeg(in, at, length)) { ok = false; break; }
            f.length = uint32_t(length);
            f.crc = le32(record + 8);
            f.input_offset = at;
            files[file_count++] = f;
        }
        if (this_state) {
            have_state = true;
            state_len = length;
        } else if (this_history) {
            history_len = length;
            /* A declared empty record is not an absent history. Both accepted
             * sizes still pass the mesh codec in valid_stage() below. */
            if (length != MESH_HISTORY_BLOB &&
                length != MESH_HISTORY_LEGACY_BLOB) { ok = false; break; }
        } else if (this_auth) {
            auth_len = length;
            if (length != kAuthSize) { ok = false; break; }
        } else if (this_identity) {
            identity_len = length;
            if (length != kIdentitySize) { ok = false; break; }
        } else if (this_reset) {
            reset_len = length;
            if (length != kResetSize) { ok = false; break; }
        } else if (!file) supported = false;
        uint32_t item = 0;
        for (size_t off = 0; off < length && ok;) {
            size_t n = length - off < sizeof(chunk) ? length - off : sizeof(chunk);
            if (!input_read(in, at + off, chunk, n)) { ok = false; break; }
            overall = crc(overall, chunk, n);
            item = crc(item, chunk, n);
            if (this_state) std::memcpy(state + off, chunk, n);
            if (this_history) std::memcpy(history + off, chunk, n);
            if (this_auth) std::memcpy(auth + off, chunk, n);
            if (this_identity) std::memcpy(identity + off, chunk, n);
            if (this_reset) std::memcpy(reset + off, chunk, n);
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
            const bool with_files = file_count != 0;
            std::memcpy(stage, with_files ? "KMS3" : "KMS2", 4);
            const size_t lengths[PartCount] = {
                state_len, history_len, auth_len, identity_len, reset_len
            };
            const uint8_t *values[PartCount] = {
                state, history, auth, identity, reset
            };
            size_t packed = with_files ? kFileHeader : kStageHeader;
            if (with_files) put16(stage + 14, uint16_t(file_count));
            for (unsigned i = 0; i < PartCount; ++i) {
                put16(stage + 4 + 2 * i, uint16_t(lengths[i]));
                if (lengths[i]) std::memcpy(stage + packed, values[i], lengths[i]);
                packed += lengths[i];
            }
            for (size_t i = 0; i < file_count; ++i) {
                stage[packed] = files[i].kind;
                put32(stage + packed + 1, files[i].id);
                put32(stage + packed + 5, files[i].length);
                put32(stage + packed + 9, files[i].crc);
                packed += kFileDescriptor;
            }
            ok = valid_stage(stage, packed, in->expected_device, b);
        }
    }
    if (ok) {
        size_t free = 0;
        const size_t stage_len = (file_count ? kFileHeader : kStageHeader) +
            state_len + history_len + auth_len + identity_len + reset_len +
            file_count * kFileDescriptor;
        const size_t entries = (stage_len + 31) / 32 + 4 +
            (state_len + 31) / 32 + 4 +
            (history_len ? (history_len + 31) / 32 + 4 : 0) +
            (auth_len ? (auth_len + 31) / 32 + 4 : 0) +
            (identity_len ? (identity_len + 31) / 32 + 4 : 0) +
            (reset_len ? (reset_len + 31) / 32 + 4 : 0);
        ok = b->target_empty(b->context) &&
            absent(b, kImportNs, kStage) &&
            absent(b, kImportNs, kJournal) &&
            b->free_entries(b->context, &free) &&
            free >= entries + 24 &&
            (!file_count || b->files_empty(b->context, file_bytes, file_count));
    }
    if (ok) {
        uint8_t journal[16];
        const size_t stage_len = (file_count ? kFileHeader : kStageHeader) +
            state_len + history_len + auth_len + identity_len + reset_len +
            file_count * kFileDescriptor;
        make_journal(journal, stage_len, crc(0, stage, stage_len));
        uint8_t *check = static_cast<uint8_t *>(std::malloc(kStageMax));
        ok = check &&
            b->write(b->context, kImportNs, kStage, stage, stage_len) &&
            same_blob(b, kImportNs, kStage, stage, stage_len, check, kStageMax);
        for (size_t i = 0; ok && i < file_count; ++i)
            ok = b->file_stage(b->context, unsigned(i), &files[i], in);
        if (ok) ok = b->write(b->context, kImportNs, kJournal,
                              journal, sizeof(journal)) &&
                     katmed_import_resume(b, in->expected_device);
        if (check) { wipe(check, kStageMax); std::free(check); }
        /* The IDF entry bound expected_device to the Factory MAC before
         * apply. An uncommitted orphan may be removed only if its stage
         * validates against that same ID. A journal requires replay. */
        if (!ok && absent(b, kImportNs, kJournal))
            (void)katmed_import_abort(b, in->expected_device);
    }
    wipe(state, KATMED_STORE_BLOB_MAX);
    wipe(history, MESH_HISTORY_BLOB);
    wipe(auth, kAuthSize);
    wipe(identity, kIdentitySize);
    wipe(reset, sizeof(reset));
    wipe(chunk, sizeof(chunk));
    if (stage) wipe(stage, kStageMax);
    std::free(state); std::free(history); std::free(auth);
    std::free(identity); std::free(stage); std::free(names);
    wipe(files, le16(header + 8) * sizeof(KatmedImportFile));
    std::free(files);
    return ok;
}
