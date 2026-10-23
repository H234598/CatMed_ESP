// Private, read-only host adapter. Compile with the unchanged product codec.
#include "../firmware/components/katmed_store/katmed_store.cpp"
#include <array>
#include <iostream>

static void id_json(EventId id) {
    std::printf("{\"device\":%" PRIu64 ",\"epoch\":%" PRIu64 ",\"sequence\":%" PRIu64 "}",
                id.device, id.epoch, id.sequence);
}
static void clock_json(ClockSample c) {
    std::printf("{\"monotonic_ms\":%" PRIu64 ",\"domain\":%" PRIu64
                ",\"utc_valid\":%s,\"utc_ms\":%" PRId64 ",\"quality\":%u}",
                c.monotonic_ms, c.domain, c.utc_valid ? "true" : "false", c.utc_ms,
                static_cast<unsigned>(c.quality));
}
static void round_json(AlarmRoundId r) {
    std::printf("{\"origin\":"); id_json(r.origin);
    std::printf(",\"ordinal\":%" PRIu64 "}", r.ordinal);
}
static void pending_json(const StorePendingConfirm &p) {
    std::printf("{\"id\":"); id_json(p.id);
    std::printf(",\"sender\":%" PRIu64 ",\"parent_revision\":{\"number\":%" PRIu64
                ",\"coordinator\":%" PRIu64 "}", p.sender,
                p.parent_revision.number, p.parent_revision.coordinator);
    std::printf(",\"round\":"); round_json(p.round);
    std::printf(",\"occurred_at\":"); clock_json(p.occurred_at);
    std::printf("}");
}
int main() {
    std::array<uint8_t,KATMED_STORE_BLOB_MAX+1> bytes{};
    std::cin.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    const size_t n=static_cast<size_t>(std::cin.gcount());
    if (n<12 || n> KATMED_STORE_BLOB_MAX ||
        (bytes[4]!=5 && bytes[4]!=7 && bytes[4]!=9 &&
         bytes[4]!=10 && bytes[4]!=11 && bytes[4]!=kCurrentVersion) || bytes[5]!=0 ||
        std::cin.bad()) return 1;
    StoreRecord record{};
    if (decode_record(bytes.data(),n,&record)!=STORE_OK) return 1;
    std::printf("{\"schema\":%u,\"language\":%u,\"mesh_name_count\":%u,\"mesh_name_next_index\":%u,\"record_device\":%" PRIu64
                ",\"boot_epoch\":%" PRIu64 ",\"next_sequence\":%" PRIu64
                ",\"configured\":%s,\"has_history\":%s,\"group_divergent\":%s",
                static_cast<unsigned>(bytes[4]), static_cast<unsigned>(record.language),
                static_cast<unsigned>(record.mesh_name_count),
                static_cast<unsigned>(record.mesh_name_next_index),
                record.device,record.epoch,record.next_sequence,
                record.provision.configured?"true":"false",record.has_history?"true":"false",
                record.group_divergent?"true":"false");
    std::printf(",\"last_confirmation\":");
    if (record.last_confirmation.valid) {
        std::printf("{\"id\":"); id_json(record.last_confirmation.id);
        std::printf(",\"round\":"); round_json(record.last_confirmation.round);
        std::printf(",\"occurred_at\":"); clock_json(record.last_confirmation.occurred_at);
        std::printf("}");
    } else std::printf("null");
    std::printf(",\"pending\":[");
    for (unsigned i=0;i<record.pending_count;++i) {
        if (i) std::printf(",");
        pending_json(record.pending[i]);
    }
    std::printf("],\"conflicts\":[");
    for (unsigned i=0;i<record.conflict_count;++i) {
        if (i) std::printf(",");
        pending_json(record.conflicts[i]);
    }
    std::printf("],\"mesh_names\":[");
    for (unsigned i=0;i<record.mesh_name_count;++i) {
        if (i) std::printf(",");
        std::printf("{\"device\":%" PRIu64 ",\"index\":%u}",
                    record.mesh_names[i].device,
                    static_cast<unsigned>(record.mesh_names[i].index));
    }
    std::printf("],\"wifi_profile_count\":%u,\"wifi_priorities\":[",
                static_cast<unsigned>(record.wifi_profile_count));
    for (unsigned i=0;i<record.wifi_profile_count;++i) {
        if (i) std::printf(",");
        std::printf("%u", static_cast<unsigned>(record.wifi_profiles[i].priority));
    }
    std::printf("],\"wifi_profiles\":[");
    for (unsigned i=0;i<record.wifi_profile_count;++i) {
        if (i) std::printf(",");
        const StoreWifiProfile &p=record.wifi_profiles[i];
        std::printf("{\"id\":%u,\"priority\":%u,\"ssid_length\":%u,"
                    "\"secured\":%s,\"password_length\":%u}",
                    p.id, static_cast<unsigned>(p.priority),
                    static_cast<unsigned>(p.ssid_length), p.secured?"true":"false",
                    static_cast<unsigned>(p.password_length));
    }
    std::printf("]}\n");
    return 0;
}
