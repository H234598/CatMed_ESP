// Host-only semantic bridge. Reuse the product Store and MeshHistory codecs.
#include "../firmware/components/katmed_store/katmed_store.cpp"
#include "katmed_mesh_status.h"

#include <array>
#include <iostream>
#include <vector>

static bool read_all(std::vector<uint8_t> &out, size_t limit)
{
    char byte;
    while (std::cin.get(byte)) {
        if (out.size() == limit) return false;
        out.push_back(static_cast<uint8_t>(byte));
    }
    return std::cin.eof() && !std::cin.bad();
}

static uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
           uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

static void write32(uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) std::cout.put(char(n >> (8 * i)));
}

static int validate()
{
    std::vector<uint8_t> input;
    if (!read_all(input, 8 + KATMED_STORE_BLOB_MAX + MESH_HISTORY_BLOB) || input.size() < 8)
        return 1;
    const size_t state_size = le32(input.data());
    if (state_size < 12 || state_size > KATMED_STORE_BLOB_MAX || input.size() < state_size + 8)
        return 1;
    const size_t history_size = le32(input.data() + 4 + state_size);
    if (history_size != 0 && history_size != MESH_HISTORY_BLOB &&
        history_size != MESH_HISTORY_LEGACY_BLOB) return 1;
    if (input.size() != 8 + state_size + history_size) return 1;

    StoreRecord record{};
    if (decode_record(input.data() + 4, state_size, &record) != STORE_OK) return 1;
    if (history_size) {
        MeshHistory history{};
        if (mesh_history_decode(&history, input.data() + 8 + state_size,
                                history_size, record.provision.group) != STORE_OK) return 1;
    }
    std::array<uint8_t, KATMED_STORE_BLOB_MAX> canonical{};
    size_t canonical_size = 0;
    if (encode_record(record, canonical.data(), &canonical_size) != STORE_OK) return 2;
    if (input[4 + 4] == 11 &&
        (canonical_size != state_size ||
         std::memcmp(canonical.data(), input.data() + 4, state_size) != 0)) return 1;
    StoreRecord check{};
    if (decode_record(canonical.data(), canonical_size, &check) != STORE_OK ||
        check.wifi_profile_count != record.wifi_profile_count ||
        check.mesh_name_count != record.mesh_name_count ||
        check.pending_count != record.pending_count ||
        check.conflict_count != record.conflict_count ||
        check.language != record.language) return 1;
    write32(static_cast<uint32_t>(canonical_size));
    std::cout.write(reinterpret_cast<const char *>(canonical.data()), canonical_size);
    write32(static_cast<uint32_t>(history_size));
    if (history_size)
        std::cout.write(reinterpret_cast<const char *>(input.data() + 8 + state_size), history_size);
    return std::cout.good() ? 0 : 1;
}

static int fixture(unsigned version)
{
    StoreRecord r{};
    r.device = 1; r.epoch = 1; r.next_sequence = 2; r.image_count = 75;
    r.shared = app_default_settings(); r.shared.revision.coordinator = 1;
    r.local = {ORIENTATION_0, DIGITAL_LARGE, 1};
    r.language = version == 11 ? KATMED_LANG_DE : KATMED_LANG_EN;
    r.group_origin = STORE_GROUP_ORIGIN_FRESH_UNBOUND;
    if (version <= 8) {
        r.group_origin = STORE_GROUP_ORIGIN_FIRST_BOUND;
        r.provision.configured = true;
        r.provision.group[0] = 1;
        r.provision.secret[0] = 2;
        r.bound_group[0] = 1;
        constexpr char ssid[] = "synthetic-legacy";
        constexpr char password[] = "synthetic-key";
        constexpr char time_source[] = "time.example";
        r.provision.ssid_length = sizeof(ssid) - 1;
        r.provision.password_length = sizeof(password) - 1;
        r.provision.time_source_length = sizeof(time_source) - 1;
        std::memcpy(r.provision.ssid, ssid, sizeof(ssid) - 1);
        std::memcpy(r.provision.password, password, sizeof(password) - 1);
        std::memcpy(r.provision.time_source, time_source, sizeof(time_source) - 1);
    }
    const ClockSample now{1000, 1, 0, false, CLOCK_UNAVAILABLE};
    const EventId boot_id{1, 1, 1};
    if (app_init(&r.history, &r.shared, &now, boot_id, r.image_count,
                 r.catalog_id) != APP_OK) return 1;
    r.has_history = true;
    r.pending_count = KATMED_STORE_PENDING_MAX;
    r.conflict_count = KATMED_STORE_CONFLICT_MAX;
    r.group_divergent = true;
    for (unsigned i = 0; i < r.pending_count; ++i) {
        StorePendingConfirm &p = r.pending[i];
        p.id = {uint64_t(100 + i), 1, 1}; p.sender = p.id.device;
        p.round = {boot_id, 1}; p.parent_revision = {1, 1}; p.occurred_at = now;
    }
    for (unsigned i = 0; i < r.conflict_count; ++i) {
        StorePendingConfirm &p = r.conflicts[i];
        p.id = {uint64_t(200 + i), 1, 1}; p.sender = p.id.device;
        p.round = {boot_id, 1}; p.parent_revision = {1, 1}; p.occurred_at = now;
    }
    std::snprintf(r.mesh_name, sizeof(r.mesh_name), "%s", KATMED_DEFAULT_DEVICE_NAME);
    r.mesh_name_state = STORE_MESH_NAME_INITIAL;
    r.mesh_name_count = KATMED_MESH_NAME_MAX_ENTRIES;
    r.mesh_name_next_index = KATMED_MESH_NAME_MAX_ENTRIES + 1;
    for (unsigned i = 0; i < r.mesh_name_count; ++i)
        r.mesh_names[i] = {uint64_t(i + 1), uint16_t(i + 1)};
    if (version == 8) {
        r.mesh_name_count = 9;
        r.mesh_name_next_index = 10;
    }
    r.wifi_profile_count = KATMED_STORE_WIFI_PROFILE_MAX;
    r.next_wifi_profile_id = KATMED_STORE_WIFI_PROFILE_MAX + 1;
    for (unsigned i = 0; i < r.wifi_profile_count; ++i) {
        StoreWifiProfile &p = r.wifi_profiles[i];
        p.id = i + 1; p.priority = i + 1;
        const int n = std::snprintf(reinterpret_cast<char *>(p.ssid), sizeof(p.ssid),
                                    "synthetic-%u", i + 1);
        if (n <= 0 || size_t(n) >= sizeof(p.ssid)) return 1;
        p.ssid_length = static_cast<uint8_t>(n);
    }
    if (version == 9) {
        r.wifi_profile_count = 0;
        r.next_wifi_profile_id = 0;
    }
    std::array<uint8_t, KATMED_STORE_BLOB_MAX> state{};
    size_t state_size = 0;
    if (encode_record(r, state.data(), &state_size, version) != STORE_OK) return 1;
    MeshHistory h{};
    std::memcpy(h.group, r.provision.group, sizeof(h.group));
    h.count = MESH_HISTORY_MAX; h.generation = 1;
    for (unsigned i = 0; i < h.count; ++i) h.ids[i] = i + 1;
    std::array<uint8_t, MESH_HISTORY_BLOB> history{};
    if (!mesh_history_encode(&h, history.data())) return 1;
    write32(static_cast<uint32_t>(state_size));
    std::cout.write(reinterpret_cast<const char *>(state.data()), state_size);
    write32(MESH_HISTORY_BLOB);
    std::cout.write(reinterpret_cast<const char *>(history.data()), history.size());
    return std::cout.good() ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (std::strcmp(argv[1], "validate") == 0) return validate();
    if (std::strcmp(argv[1], "synthetic-fixture") == 0) return fixture(11);
    if (std::strcmp(argv[1], "synthetic-v10-fixture") == 0) return fixture(10);
    if (std::strcmp(argv[1], "synthetic-v9-fixture") == 0) return fixture(9);
    if (std::strcmp(argv[1], "synthetic-v8-fixture") == 0) return fixture(8);
    if (std::strcmp(argv[1], "synthetic-v7-fixture") == 0) return fixture(7);
    if (std::strcmp(argv[1], "synthetic-v5-fixture") == 0) return fixture(5);
    return 2;
}
