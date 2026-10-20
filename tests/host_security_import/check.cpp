#include "katmed_import.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

struct Memory {
    std::map<std::string, std::vector<uint8_t>> values;
    std::vector<uint8_t> capsule;
    bool fail_live_once = false;
    bool fail_history_once = false;
    bool fail_live_read_once = false;
    bool fail_journal_once = false;
    bool fail_erase_once = false;
    bool fail_stage_erase_once = false;
    size_t available_entries = 600;
    unsigned writes = 0;
    unsigned erases = 0;
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
    if (m.fail_live_read_once && id(ns, key) == "katmed/state" && out) {
        m.fail_live_read_once = false;
        return KATMED_IMPORT_READ_ERROR;
    }
    auto it = m.values.find(id(ns, key));
    if (it == m.values.end()) return KATMED_IMPORT_READ_ABSENT;
    if (out && *n < it->second.size()) return KATMED_IMPORT_READ_ERROR;
    if (out) std::memcpy(out, it->second.data(), it->second.size());
    *n = it->second.size();
    return KATMED_IMPORT_READ_OK;
}
static bool write_blob(void *context, const char *ns, const char *key,
                       const uint8_t *data, size_t n) {
    auto &m = *static_cast<Memory *>(context);
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
static bool free_entries(void *context, size_t *out) {
    *out = static_cast<Memory *>(context)->available_entries;
    return true;
}
static KatmedImportBackend backend(Memory &m) {
    return {&m, read_blob, write_blob, erase_blob, target_empty, free_entries};
}
static KatmedImportInput input(Memory &m) {
    return {&m, m.capsule.size(), 1, input_read};
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
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
            m.values.at("katmed/state")[4] == 11 &&
            m.values.at("katmed/state").back() == 1 &&
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
            !katmed_import_resume(&b, 1) &&
            katmed_import_abort(&b) &&
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
        ok = !katmed_import_apply(&in, &b) && m.writes == 0;
    }
    if (!ok) {
        std::fputs("synthetic import scenario failed\n", stderr);
        return 1;
    }
    return 0;
}
