#include "katmed_import.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <zlib.h>

struct Memory {
    std::map<std::string, std::vector<uint8_t>> values;
    std::set<std::string> u8_keys;
    std::vector<uint8_t> capsule;
    bool fail_live_once = false;
    bool fail_history_once = false;
    bool fail_live_read_once = false;
    bool fail_journal_once = false;
    bool fail_erase_once = false;
    bool fail_stage_erase_once = false;
    bool reject_web_once = false;
    std::string fail_write_key_once;
    std::string fail_read_key_once;
    size_t available_entries = 600;
    unsigned writes = 0;
    unsigned erases = 0;
    std::map<std::string, std::vector<uint8_t>> staged_files;
    std::map<std::string, std::vector<uint8_t>> final_files;
    size_t fs_free = 576 * 1024;
    int fail_publish_at = -1;
    int fail_stage_at = -1;
    bool fail_file_abort_once = false;
    bool corrupt_stage_on_file_error = false;
    unsigned file_aborts = 0;
};
static std::string id(const char *ns, const char *key) {
    return std::string(ns) + "/" + key;
}
static bool input_read(void *context, size_t at, uint8_t *out, size_t n) {
    auto &m = *static_cast<Memory *>(context);
    if (at > m.capsule.size() || n > m.capsule.size() - at) return false;
    std::memcpy(out, m.capsule.data() + at, n);
    return true;
}
static KatmedImportRead read_blob(void *context, const char *ns, const char *key,
                                  uint8_t *out, size_t *n) {
    auto &m = *static_cast<Memory *>(context);
    if (out && m.fail_read_key_once == id(ns, key)) {
        m.fail_read_key_once.clear();
        return KATMED_IMPORT_READ_ERROR;
    }
    if (m.fail_live_read_once && id(ns, key) == "katmed/state" && out) {
        m.fail_live_read_once = false;
        return KATMED_IMPORT_READ_ERROR;
    }
    auto it = m.values.find(id(ns, key));
    if (it == m.values.end()) return KATMED_IMPORT_READ_ABSENT;
    if (m.u8_keys.count(id(ns, key))) return KATMED_IMPORT_READ_ERROR;
    if (out && *n < it->second.size()) return KATMED_IMPORT_READ_ERROR;
    if (out) std::memcpy(out, it->second.data(), it->second.size());
    *n = it->second.size();
    return KATMED_IMPORT_READ_OK;
}
static bool write_blob(void *context, const char *ns, const char *key,
                       const uint8_t *data, size_t n) {
    auto &m = *static_cast<Memory *>(context);
    if (m.fail_write_key_once == id(ns, key)) {
        m.fail_write_key_once.clear();
        return false;
    }
    if (m.fail_live_once && id(ns, key) == "katmed/state") {
        m.fail_live_once = false; return false;
    }
    if (m.fail_history_once && id(ns, key) == "katmed/mesh_history") {
        m.fail_history_once = false; return false;
    }
    if (m.fail_journal_once && id(ns, key) == "katmed_imp/journal") {
        m.fail_journal_once = false; return false;
    }
    ++m.writes;
    m.values[id(ns, key)] = std::vector<uint8_t>(data, data + n);
    return true;
}
static KatmedImportRead read_u8(void *context, const char *ns, const char *key,
                                uint8_t *out) {
    auto &m = *static_cast<Memory *>(context);
    auto it = m.values.find(id(ns, key));
    if (it == m.values.end()) return KATMED_IMPORT_READ_ABSENT;
    if (!m.u8_keys.count(id(ns, key)) || it->second.size() != 1)
        return KATMED_IMPORT_READ_ERROR;
    *out = it->second[0];
    return KATMED_IMPORT_READ_OK;
}
static bool write_u8(void *context, const char *ns, const char *key,
                     uint8_t value) {
    auto &m = *static_cast<Memory *>(context);
    if (m.fail_write_key_once == id(ns, key)) {
        m.fail_write_key_once.clear();
        return false;
    }
    ++m.writes;
    m.u8_keys.insert(id(ns, key));
    m.values[id(ns, key)] = {value};
    return true;
}
static bool erase_blob(void *context, const char *ns, const char *key) {
    auto &m = *static_cast<Memory *>(context);
    if (m.fail_erase_once) { m.fail_erase_once = false; return false; }
    if (m.fail_stage_erase_once && id(ns, key) == "katmed_imp/stage") {
        m.fail_stage_erase_once = false; return false;
    }
    ++m.erases;
    m.values.erase(id(ns, key));
    return true;
}
static bool target_empty(void *context) {
    return static_cast<Memory *>(context)->values.empty();
}
static bool available_entries(void *context, size_t *out) {
    *out = static_cast<Memory *>(context)->available_entries;
    return true;
}
static bool validate_web(void *context, const uint8_t *auth, size_t auth_len,
                         const uint8_t *identity, size_t identity_len) {
    auto &m = *static_cast<Memory *>(context);
    if (m.reject_web_once) { m.reject_web_once = false; return false; }
    return (!auth_len || (auth && auth_len == 1112)) &&
        (!identity_len || (identity && identity_len == 1288)) &&
        (!auth_len || identity_len);
}
static std::string file_id(const KatmedImportFile *f) {
    return std::to_string(f->kind) + "/" + std::to_string(f->id);
}
static bool file_equal(const std::vector<uint8_t> &bytes,
                       const KatmedImportFile *f) {
    return bytes.size() == f->length &&
        uint32_t(crc32(0, bytes.data(), bytes.size())) == f->crc;
}
static bool files_empty(void *context, size_t bytes, size_t count) {
    auto &m = *static_cast<Memory *>(context);
    return m.staged_files.empty() && m.final_files.empty() &&
        m.fs_free >= bytes + 32768 + count * 256;
}
static bool file_stage(void *context, unsigned ordinal, const KatmedImportFile *f,
                       const KatmedImportInput *in) {
    auto &m = *static_cast<Memory *>(context);
    if (int(ordinal) == m.fail_stage_at) {
        m.fail_stage_at = -1;
        if (m.corrupt_stage_on_file_error)
            m.values.at("katmed_imp/stage")[16] ^= 1;
        return false;
    }
    if (m.staged_files.count(file_id(f)) || m.final_files.count(file_id(f))) return false;
    std::vector<uint8_t> bytes(f->length);
    if (!in->read(in->context, f->input_offset, bytes.data(), bytes.size()) ||
        !file_equal(bytes, f)) return false;
    m.staged_files[file_id(f)] = std::move(bytes);
    return true;
}
static bool file_final(void *context, unsigned, const KatmedImportFile *f) {
    auto &m = *static_cast<Memory *>(context);
    auto it = m.final_files.find(file_id(f));
    return it != m.final_files.end() && file_equal(it->second, f) &&
        m.staged_files.count(file_id(f)) == 0;
}
static bool file_publish(void *context, unsigned ordinal, const KatmedImportFile *f) {
    auto &m = *static_cast<Memory *>(context);
    if (int(ordinal) == m.fail_publish_at) { m.fail_publish_at = -1; return false; }
    const auto final = m.final_files.find(file_id(f));
    if (final != m.final_files.end()) {
        if (!file_equal(final->second, f)) return false;
        m.staged_files.erase(file_id(f));
        return true;
    }
    const auto staged = m.staged_files.find(file_id(f));
    if (staged == m.staged_files.end() || !file_equal(staged->second, f)) return false;
    m.final_files[file_id(f)] = staged->second;
    m.staged_files.erase(staged);
    return true;
}
static bool file_abort(void *context, unsigned, const KatmedImportFile *f) {
    auto &m = *static_cast<Memory *>(context);
    if (m.fail_file_abort_once) { m.fail_file_abort_once = false; return false; }
    if (m.final_files.count(file_id(f))) return false;
    ++m.file_aborts;
    m.staged_files.erase(file_id(f));
    return true;
}
static KatmedImportBackend backend(Memory &m) {
    return {&m, read_blob, write_blob, read_u8, write_u8,
            erase_blob, target_empty, available_entries,
            validate_web, files_empty, file_stage, file_publish, file_final,
            file_abort};
}
static KatmedImportInput input(Memory &m) {
    return {&m, m.capsule.size(), 1, input_read};
}
int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) return 2;
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) return 2;
    Memory m;
    m.capsule.assign(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
    auto b = backend(m);
    auto in = input(m);
    const std::string mode = argv[2];
    bool ok = false;
    if (mode == "success") {
        ok = katmed_import_apply(&in, &b) &&
            m.values.count("katmed/state") == 1 &&
            m.values.at("katmed/state")[4] == 15 &&
            m.values.at("katmed/state").at(m.values.at("katmed/state").size() - 32) == 1 &&
            m.values.at("katmed/state").back() == 0 &&
            m.values.count("katmed_imp/journal") == 0 &&
            m.values.count("katmed_imp/stage") == 0 &&
            katmed_import_resume(&b, 1);
    } else if (mode == "replay") {
        m.fail_live_once = true;
        ok = !katmed_import_apply(&in, &b) &&
            m.values.count("katmed_imp/journal") == 1 &&
            m.values.count("katmed/state") == 0 &&
            katmed_import_resume(&b, 1) &&
            m.values.count("katmed/state") == 1 &&
            m.values.count("katmed_imp/journal") == 0;
    } else if (mode == "replay_wrong_device") {
        m.fail_live_once = true;
        ok = !katmed_import_apply(&in, &b) &&
            m.values.count("katmed_imp/journal") == 1 &&
            m.values.count("katmed_imp/stage") == 1;
        const unsigned writes = m.writes, erases = m.erases;
        ok = ok && !katmed_import_resume(&b, 2) &&
            m.writes == writes && m.erases == erases &&
            m.values.count("katmed/state") == 0 &&
            m.values.count("katmed_imp/journal") == 1 &&
            m.values.count("katmed_imp/stage") == 1 &&
            katmed_import_resume(&b, 1) &&
            m.values.count("katmed/state") == 1 &&
            m.values.count("katmed_imp/journal") == 0 &&
            m.values.count("katmed_imp/stage") == 0;
    } else if (mode == "history") {
        ok = katmed_import_apply(&in, &b) &&
            m.values.count("katmed/state") == 1 &&
            m.values.count("katmed/mesh_history") == 1 &&
            m.values.count("katmed_imp/journal") == 0;
    } else if (mode == "full_nvs") {
        ok = katmed_import_apply(&in, &b) &&
            m.values.count("katmed/state") == 1 &&
            m.values.count("katmed/mesh_history") == 1 &&
            m.values.count("katmed_web/identity") == 1 &&
            m.values.count("katmed_web/auth") == 1 &&
            m.values.count("katmed_reset/journal") == 1 &&
            m.values.at("katmed_web/auth")[8] == 0 &&
            m.values.at("katmed_web/auth")[24] == 'V' &&
            m.values.at("katmed_web/identity")[8] == 'K' &&
            m.values.at("katmed_web/identity")[264] == 'C' &&
            m.values.count("katmed_imp/journal") == 0 &&
            m.values.count("katmed_imp/stage") == 0 &&
            katmed_import_resume(&b, 1);
    } else if (mode == "identity_only") {
        ok = katmed_import_apply(&in, &b) &&
            m.values.count("katmed_web/identity") == 1 &&
            m.values.count("katmed_web/auth") == 0 &&
            m.values.count("katmed_reset/journal") == 0 &&
            m.values.count("katmed_imp/journal") == 0;
    } else if (mode == "bridge") {
        ok = katmed_import_apply(&in, &b) &&
            m.values.at("netif_ip_info/WIFI_AP_DEF").size() == 12 &&
            m.values.at("netif_ip_info/WIFI_STA_DEF").size() == 12 &&
            m.u8_keys.count("netif_ip_info/WIFI_AP__check") == 1 &&
            m.u8_keys.count("netif_ip_info/WIFI_STA_check") == 1 &&
            m.values.at("netif_ip_info/WIFI_AP__check")[0] == 1 &&
            m.values.at("netif_ip_info/WIFI_STA_check")[0] == 0 &&
            katmed_import_resume(&b, 1);
    } else if (mode == "bridge_replay") {
        m.fail_write_key_once = "netif_ip_info/WIFI_AP__check";
        ok = !katmed_import_apply(&in, &b) &&
            m.values.count("katmed_imp/journal") == 1 &&
            m.values.count("netif_ip_info/WIFI_AP_DEF") == 1 &&
            m.values.count("netif_ip_info/WIFI_AP__check") == 0;
        const unsigned writes = m.writes, erases = m.erases;
        ok = ok && !katmed_import_resume(&b, 2) &&
            m.writes == writes && m.erases == erases;
        m.values["netif_ip_info/WIFI_AP__check"] = {1}; // Wrong NVS type: blob.
        ok = ok && !katmed_import_resume(&b, 1) &&
            m.writes == writes && m.erases == erases &&
            m.values.count("katmed_imp/journal") == 1;
        m.values.erase("netif_ip_info/WIFI_AP__check");
        ok = ok &&
            katmed_import_resume(&b, 1) &&
            m.u8_keys.count("netif_ip_info/WIFI_AP__check") == 1 &&
            m.values.count("katmed_imp/journal") == 0;
    } else if (mode == "bridge_orphan") {
        ok = katmed_import_apply(&in, &b) &&
            m.values.count("netif_ip_info/WIFI_AP_DEF") == 0 &&
            m.u8_keys.count("netif_ip_info/WIFI_AP__check") == 1;
    } else if (mode == "bridge_abort") {
        m.fail_journal_once = true;
        m.fail_erase_once = true;
        ok = !katmed_import_apply(&in, &b) &&
            m.values.count("katmed_imp/stage") == 1 &&
            m.values.count("katmed_imp/journal") == 0 &&
            m.u8_keys.empty();
        const unsigned writes = m.writes, erases = m.erases;
        ok = ok && !katmed_import_abort(&b, 2) &&
            m.writes == writes && m.erases == erases &&
            m.values.count("katmed_imp/stage") == 1 &&
            katmed_import_abort(&b, 1) &&
            m.values.count("katmed_imp/stage") == 0 &&
            katmed_import_resume(&b, 1);
    } else if (mode == "bridge_capacity") {
        // Full v11 State, Mesh history and four typed Bridge entries.
        m.available_entries = 440;
        ok = !katmed_import_apply(&in, &b) &&
            m.writes == 0 && m.erases == 0 && m.values.empty();
        m.available_entries = 441;
        ok = ok && katmed_import_apply(&in, &b) &&
            m.values.count("netif_ip_info/WIFI_AP_DEF") == 1 &&
            m.values.count("netif_ip_info/WIFI_STA_DEF") == 1 &&
            m.u8_keys.count("netif_ip_info/WIFI_AP__check") == 1 &&
            m.u8_keys.count("netif_ip_info/WIFI_STA_check") == 1;
    } else if (mode == "mixed_success") {
        ok = katmed_import_apply(&in, &b) &&
            m.final_files.size() == 2 &&
            m.final_files.count("3/6") == 1 &&
            m.final_files.count("1/1") == 1 &&
            m.values.count("katmed/state") == 1;
    } else if (mode == "acme_success") {
        ok = katmed_import_apply(&in, &b) &&
            m.final_files.size() == 7 && m.staged_files.empty() &&
            m.final_files.count("3/6") == 1 &&
            m.values.count("katmed/state") == 1 &&
            m.values.count("katmed_imp/journal") == 0 &&
            katmed_import_resume(&b, 1) && !katmed_import_apply(&in, &b);
    } else if (mode == "acme_replay") {
        m.fail_publish_at = 6;
        ok = !katmed_import_apply(&in, &b) &&
            m.values.count("katmed_imp/journal") == 1 &&
            m.values.count("katmed/state") == 0 &&
            m.staged_files.count("3/6") == 1;
        const unsigned writes = m.writes, erases = m.erases;
        ok = ok && !katmed_import_resume(&b, 2) &&
            m.writes == writes && m.erases == erases &&
            katmed_import_resume(&b, 1) &&
            m.final_files.size() == 7 && m.staged_files.empty() &&
            m.values.count("katmed/state") == 1;
    } else if (mode == "files_success") {
        ok = katmed_import_apply(&in, &b) &&
            m.final_files.size() == 2 && m.staged_files.empty() &&
            m.values.count("katmed/state") == 1 &&
            m.values.at("katmed/state").size() == 4096 &&
            m.values.at("katmed/state")[4] == 15 &&
            m.values.at("katmed/state").at(m.values.at("katmed/state").size() - 32) == 1 &&
            m.values.at("katmed/state").back() == 0 &&
            m.values.count("katmed_web/identity") == 1 &&
            m.values.count("katmed_web/auth") == 1 &&
            m.values.count("katmed_reset/journal") == 1 &&
            m.values.count("katmed_imp/journal") == 0 &&
            katmed_import_resume(&b, 1) &&
            !katmed_import_apply(&in, &b) && m.final_files.size() == 2;
    } else if (mode == "files_replay") {
        m.fail_publish_at = 1;
        ok = !katmed_import_apply(&in, &b) &&
            m.final_files.size() == 1 && m.staged_files.size() == 1 &&
            m.values.count("katmed_imp/journal") == 1 &&
            m.values.count("katmed/state") == 0;
        const unsigned writes = m.writes, erases = m.erases;
        ok = ok && !katmed_import_resume(&b, 2) &&
            m.writes == writes && m.erases == erases &&
            katmed_import_resume(&b, 1) &&
            m.final_files.size() == 2 && m.staged_files.empty() &&
            m.values.count("katmed/state") == 1 &&
            m.values.count("katmed_imp/journal") == 0;
    } else if (mode == "files_missing") {
        m.fail_publish_at = 1;
        ok = !katmed_import_apply(&in, &b) && m.staged_files.size() == 1;
        m.staged_files.clear();
        const unsigned writes = m.writes, erases = m.erases;
        ok = ok && !katmed_import_resume(&b, 1) &&
            m.writes == writes && m.erases == erases &&
            m.values.count("katmed_imp/journal") == 1 &&
            m.values.count("katmed/state") == 0;
    } else if (mode == "files_stage_error") {
        m.fail_stage_at = 1;
        ok = !katmed_import_apply(&in, &b) &&
            m.staged_files.empty() && m.final_files.empty() &&
            m.values.count("katmed_imp/journal") == 0 &&
            m.values.count("katmed_imp/stage") == 0 &&
            m.values.count("katmed/state") == 0;
    } else if (mode == "files_orphan_guard") {
        m.fail_stage_at = 1;
        m.fail_file_abort_once = true;
        ok = !katmed_import_apply(&in, &b) &&
            m.values.count("katmed_imp/stage") == 1 &&
            m.values.count("katmed_imp/journal") == 0 &&
            m.staged_files.size() == 1 && m.final_files.empty();
        const auto stage = m.values.at("katmed_imp/stage");
        const auto files = m.staged_files;
        const unsigned writes = m.writes, erases = m.erases,
                       file_aborts = m.file_aborts;
        ok = ok && !katmed_import_resume(&b, 2) &&
            !katmed_import_abort(&b, 2) &&
            m.writes == writes && m.erases == erases &&
            m.file_aborts == file_aborts &&
            m.values.at("katmed_imp/stage") == stage &&
            m.staged_files == files;
        const size_t state_len = size_t(stage[4]) | size_t(stage[5]) << 8;
        const size_t history_len = size_t(stage[6]) | size_t(stage[7]) << 8;
        const size_t auth_len = size_t(stage[8]) | size_t(stage[9]) << 8;
        const size_t identity_len = size_t(stage[10]) | size_t(stage[11]) << 8;
        const size_t auth_at = 16 + state_len + history_len;
        const size_t reset_at = auth_at + auth_len + identity_len;
        for (size_t off : {size_t(16), auth_at, reset_at}) {
            m.values.at("katmed_imp/stage") = stage;
            m.values.at("katmed_imp/stage")[off] ^= 1;
            const auto damaged = m.values.at("katmed_imp/stage");
            ok = ok && !katmed_import_abort(&b, 1) &&
                m.writes == writes && m.erases == erases &&
                m.file_aborts == file_aborts &&
                m.values.at("katmed_imp/stage") == damaged &&
                m.staged_files == files;
        }
        m.values.at("katmed_imp/stage") = stage;
        m.reject_web_once = true;
        ok = ok && !katmed_import_abort(&b, 1) &&
            m.writes == writes && m.erases == erases &&
            m.file_aborts == file_aborts && m.staged_files == files;
        ok = ok && katmed_import_abort(&b, 1) &&
            m.values.count("katmed_imp/stage") == 0 &&
            m.staged_files.empty() && m.final_files.empty() &&
            m.values.count("katmed/state") == 0 &&
            katmed_import_resume(&b, 1);
    } else if (mode == "files_auto_abort_damaged") {
        m.fail_stage_at = 1;
        m.corrupt_stage_on_file_error = true;
        ok = !katmed_import_apply(&in, &b) &&
            m.values.count("katmed_imp/stage") == 1 &&
            m.values.count("katmed_imp/journal") == 0 &&
            m.staged_files.size() == 1 && m.final_files.empty() &&
            m.file_aborts == 0 && m.erases == 0;
        const unsigned writes = m.writes, erases = m.erases;
        ok = ok && !katmed_import_abort(&b, 1) &&
            m.writes == writes && m.erases == erases &&
            m.file_aborts == 0 && m.staged_files.size() == 1 &&
            m.values.count("katmed_imp/stage") == 1;
    } else if (mode == "files_no_space" || mode == "files_zero_space") {
        m.fs_free = mode == "files_zero_space" ? 0 : 8 * 1024;
        ok = !katmed_import_apply(&in, &b) &&
            m.writes == 0 && m.erases == 0 && m.values.empty() &&
            m.staged_files.empty();
    } else if (mode == "files_occupied") {
        m.final_files["1/7"] = {1, 2, 3};
        ok = !katmed_import_apply(&in, &b) &&
            m.writes == 0 && m.erases == 0 && m.values.empty() &&
            m.final_files.at("1/7") == std::vector<uint8_t>({1, 2, 3});
    } else if (mode == "files_many") {
        ok = katmed_import_apply(&in, &b) &&
            m.final_files.size() == 127 && m.staged_files.empty() &&
            m.values.count("katmed/state") == 1 &&
            m.values.count("katmed_imp/journal") == 0 &&
            katmed_import_resume(&b, 1);
    } else if (mode == "capacity_97" || mode == "capacity_123") {
        const size_t images = mode == "capacity_97" ? 97 : 123;
        const size_t required = mode == "capacity_97" ? 632 : 642;
        m.available_entries = required - 1;
        ok = !katmed_import_apply(&in, &b) &&
            m.writes == 0 && m.erases == 0 && m.values.empty() &&
            m.staged_files.empty() && m.final_files.empty();
        m.available_entries = 756; /* Seven IDF pages minus its reserved page. */
        ok = ok && katmed_import_apply(&in, &b) &&
            m.final_files.size() == images && m.staged_files.empty() &&
            m.values.count("katmed/state") == 1 &&
            m.values.count("katmed/mesh_history") == 1 &&
            m.values.count("katmed_web/auth") == 1 &&
            m.values.count("katmed_web/identity") == 1 &&
            m.values.count("katmed_reset/journal") == 1 &&
            m.values.count("katmed_imp/journal") == 0 &&
            katmed_import_resume(&b, 1);
    } else if (mode == "legacy_cleanup") {
        const size_t path_len = size_t(m.capsule[16]) | size_t(m.capsule[17]) << 8;
        const size_t data_len = size_t(m.capsule[20]) |
            size_t(m.capsule[21]) << 8 | size_t(m.capsule[22]) << 16 |
            size_t(m.capsule[23]) << 24;
        const size_t data_at = 16 + 12 + path_len;
        if (data_len > 4096 || data_at + data_len != m.capsule.size()) return 2;
        const std::vector<uint8_t> state(m.capsule.begin() + data_at,
                                         m.capsule.end());
        std::vector<uint8_t> stage = {
            uint8_t(data_len), uint8_t(data_len >> 8), 0, 0,
        };
        stage.insert(stage.end(), state.begin(), state.end());
        m.values["katmed/state"] = state;
        m.values["katmed_imp/stage"] = stage;
        const unsigned writes = m.writes, erases = m.erases;
        ok = !katmed_import_resume(&b, 2) &&
            m.writes == writes && m.erases == erases &&
            m.values.at("katmed_imp/stage") == stage &&
            katmed_import_resume(&b, 1) &&
            m.values.count("katmed/state") == 1 &&
            m.values.count("katmed_imp/stage") == 0;
    } else if (mode == "full_space") {
        m.available_entries = 589;
        ok = !katmed_import_apply(&in, &b) &&
            m.writes == 0 && m.erases == 0 && m.values.empty();
    } else if (mode == "reject_web") {
        m.reject_web_once = true;
        ok = !katmed_import_apply(&in, &b) &&
            m.writes == 0 && m.erases == 0 && m.values.empty();
    } else if (mode == "full_wrong_replay_id") {
        m.fail_write_key_once = "katmed_web/auth";
        ok = !katmed_import_apply(&in, &b) &&
            m.values.count("katmed_imp/journal") == 1 &&
            m.values.count("katmed_web/identity") == 1 &&
            m.values.count("katmed_web/auth") == 0;
        const unsigned writes = m.writes, erases = m.erases;
        ok = ok && !katmed_import_resume(&b, 2) &&
            m.writes == writes && m.erases == erases &&
            m.values.count("katmed_imp/journal") == 1 &&
            katmed_import_resume(&b, 1) &&
            m.values.count("katmed_web/auth") == 1 &&
            m.values.count("katmed_reset/journal") == 1 &&
            m.values.count("katmed_imp/journal") == 0;
    } else if (mode == "live_conflict") {
        m.fail_write_key_once = "katmed_web/auth";
        ok = !katmed_import_apply(&in, &b) &&
            m.values.count("katmed_imp/journal") == 1 &&
            m.values.count("katmed_web/identity") == 1;
        m.values.at("katmed_web/identity")[8] ^= 1;
        const unsigned writes = m.writes, erases = m.erases;
        ok = ok && !katmed_import_resume(&b, 1) &&
            m.writes == writes && m.erases == erases &&
            m.values.count("katmed_imp/journal") == 1;
    } else if (mode == "fail_write" || mode == "fail_readback") {
        if (argc != 4) return 2;
        const std::string key = argv[3];
        if (mode == "fail_write") m.fail_write_key_once = key;
        else m.fail_read_key_once = key;
        ok = !katmed_import_apply(&in, &b);
        const bool live = key != "katmed_imp/stage" &&
                          key != "katmed_imp/journal";
        if (live || (mode == "fail_readback" &&
                     key == "katmed_imp/journal")) {
            ok = ok && m.values.count("katmed_imp/journal") == 1 &&
                katmed_import_resume(&b, 1) &&
                m.values.count("katmed_web/auth") == 1 &&
                m.values.count("katmed_web/identity") == 1 &&
                m.values.count("katmed_reset/journal") == 1 &&
                m.values.count("katmed_imp/journal") == 0;
        } else {
            ok = ok && m.values.count("katmed_imp/journal") == 0 &&
                m.values.count("katmed/state") == 0 &&
                m.values.count("katmed_web/auth") == 0 &&
                katmed_import_resume(&b, 1);
        }
    } else if (mode == "replay_history") {
        m.fail_history_once = true;
        ok = !katmed_import_apply(&in, &b) &&
            m.values.count("katmed/state") == 1 &&
            m.values.count("katmed/mesh_history") == 0 &&
            m.values.count("katmed_imp/journal") == 1 &&
            katmed_import_resume(&b, 1) &&
            m.values.count("katmed/mesh_history") == 1 &&
            m.values.count("katmed_imp/journal") == 0;
    } else if (mode == "readback") {
        m.fail_live_read_once = true;
        ok = !katmed_import_apply(&in, &b) &&
            m.values.count("katmed_imp/journal") == 1 &&
            katmed_import_resume(&b, 1) &&
            m.values.count("katmed_imp/journal") == 0;
    } else if (mode == "orphan") {
        m.fail_journal_once = true;
        m.fail_erase_once = true;
        ok = !katmed_import_apply(&in, &b) &&
            m.values.count("katmed_imp/stage") == 1 &&
            !katmed_import_resume(&b, 1);
        const unsigned writes = m.writes, erases = m.erases;
        ok = ok && !katmed_import_abort(&b, 2) &&
            m.writes == writes && m.erases == erases &&
            m.values.count("katmed_imp/stage") == 1 &&
            katmed_import_abort(&b, 1) &&
            katmed_import_resume(&b, 1);
    } else if (mode == "cleanup") {
        m.fail_stage_erase_once = true;
        ok = !katmed_import_apply(&in, &b) &&
            m.values.count("katmed/state") == 1 &&
            m.values.count("katmed_imp/journal") == 0 &&
            m.values.count("katmed_imp/stage") == 1 &&
            katmed_import_resume(&b, 1) &&
            m.values.count("katmed_imp/stage") == 0;
    } else if (mode == "cleanup_wrong_device") {
        m.fail_stage_erase_once = true;
        ok = !katmed_import_apply(&in, &b) &&
            m.values.count("katmed/state") == 1 &&
            m.values.count("katmed_imp/journal") == 0 &&
            m.values.count("katmed_imp/stage") == 1;
        const unsigned writes = m.writes, erases = m.erases;
        ok = ok && !katmed_import_resume(&b, 2) &&
            m.writes == writes && m.erases == erases &&
            m.values.count("katmed_imp/stage") == 1 &&
            katmed_import_resume(&b, 1) &&
            m.values.count("katmed_imp/stage") == 0;
    } else if (mode == "bad_journal") {
        m.fail_live_once = true;
        ok = !katmed_import_apply(&in, &b);
        if (ok) {
            m.values["katmed_imp/journal"].back() ^= 1;
            const unsigned writes = m.writes, erases = m.erases;
            ok = !katmed_import_resume(&b, 1) &&
                m.values.count("katmed/state") == 0 &&
                m.values.count("katmed_imp/journal") == 1 &&
                m.values.count("katmed_imp/stage") == 1 &&
                m.writes == writes && m.erases == erases;
        }
    } else if (mode == "space") {
        m.available_entries = 1;
        ok = !katmed_import_apply(&in, &b) && m.writes == 0;
    } else if (mode == "occupied") {
        m.values["wifi/sdk"] = {1};
        ok = !katmed_import_apply(&in, &b) && m.writes == 0 &&
            m.values["wifi/sdk"] == std::vector<uint8_t>{1};
    } else if (mode == "corrupt") {
        m.capsule.back() ^= 1;
        ok = !katmed_import_apply(&in, &b) && m.writes == 0;
    } else if (mode == "version") {
        m.capsule[3] = '2';
        ok = !katmed_import_apply(&in, &b) && m.writes == 0;
    } else if (mode == "wrong_device") {
        in.expected_device = 2;
        ok = !katmed_import_apply(&in, &b) && m.writes == 0;
    } else if (mode == "reject") {
        ok = !katmed_import_apply(&in, &b) &&
            m.writes == 0 && m.erases == 0 && m.values.empty();
    }
    if (!ok) {
        std::fputs("synthetic import scenario failed\n", stderr);
        return 1;
    }
    return 0;
}
