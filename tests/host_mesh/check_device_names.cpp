#include "katmed_store.h"
#include "katmed_mesh_payload.h"

#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include <zlib.h>

namespace {
struct Memory {
    std::vector<uint8_t> blob;
    enum Fault { NONE, BEFORE, AFTER } fault = NONE;
};

StoreResult read_blob(void *context, uint8_t *out, size_t *length)
{
    auto &memory = *static_cast<Memory *>(context);
    if (memory.blob.empty()) return STORE_NOT_FOUND;
    if (*length < memory.blob.size()) return STORE_FULL;
    std::memcpy(out, memory.blob.data(), memory.blob.size());
    *length = memory.blob.size();
    return STORE_OK;
}

StoreResult write_blob(void *context, const uint8_t *bytes, size_t length)
{
    auto &memory = *static_cast<Memory *>(context);
    if (memory.fault == Memory::BEFORE) return STORE_IO;
    memory.blob.assign(bytes, bytes + length);
    if (memory.fault == Memory::AFTER) return STORE_COMMIT_UNKNOWN;
    return STORE_OK;
}

StoreBackend backend(Memory &memory) { return {&memory, read_blob, write_blob}; }

uint8_t catalog[KATMED_CATALOG_BYTES] = {0x42};

Store open(Memory &memory, uint64_t device)
{
    Store store{};
    StoreRecovery recovery{};
    assert(store_open(&store, backend(memory), device, catalog, 3, &recovery) == STORE_OK);
    return store;
}

void assert_name(const Store &store, const char *expected, uint16_t index)
{
    assert(std::strcmp(store_mesh_name(&store), expected) == 0);
    assert(store_mesh_name_index(&store) == index);
}

void test_fresh_assignment_and_retry()
{
    Memory root_memory;
    Store root = open(root_memory, 1);
    assert_name(root, "KatmedESP", 0);

    assert(store_mesh_name_assign(&root, 1, 1) == STORE_OK);
    assert_name(root, "KatmedESP-001", 1);
    assert(store_mesh_name_assign(&root, 2, 2) == STORE_OK);
    assert(store_mesh_name_assign(&root, 2, 2) == STORE_DUPLICATE);
    assert(store_mesh_name_assign(&root, 3, 3) == STORE_OK);
    assert(store_mesh_name_next_index(&root) == 4);

    Store reopened = open(root_memory, 1);
    assert_name(reopened, "KatmedESP-001", 1);
    assert(store_mesh_name_has_device(&reopened, 2));

    const StoreMeshNameEntry map[] = {{1, 1}, {2, 2}, {3, 3}};
    Memory leaf_memory;
    Store leaf = open(leaf_memory, 2);
    assert_name(leaf, "KatmedESP", 0);
    assert(store_mesh_name_adopt(&leaf, map, 3) == STORE_OK);
    assert_name(leaf, "KatmedESP-002", 2);
    assert(store_mesh_name_adopt(&leaf, map, 3) == STORE_DUPLICATE);

    /* A lost assignment response is retried byte-identically and cannot
     * allocate a second index. */
    assert(store_mesh_name_adopt(&leaf, map, 3) == STORE_DUPLICATE);
}

void test_commit_before_after_and_unknown()
{
    Memory before_memory;
    Store before = open(before_memory, 10);
    before_memory.fault = Memory::BEFORE;
    assert(store_mesh_name_assign(&before, 10, 1) == STORE_IO);
    assert_name(before, "KatmedESP", 0);
    before_memory.fault = Memory::NONE;
    Store before_reopened = open(before_memory, 10);
    assert_name(before_reopened, "KatmedESP", 0);

    Memory after_memory;
    Store after = open(after_memory, 11);
    after_memory.fault = Memory::AFTER;
    assert(store_mesh_name_assign(&after, 11, 1) == STORE_COMMIT_UNKNOWN);
    assert(after.uncertain);
    assert_name(after, "KatmedESP", 0);
    after_memory.fault = Memory::NONE;
    Store after_reopened = open(after_memory, 11);
    assert_name(after_reopened, "KatmedESP-001", 1);
}

void test_concurrent_first_joins()
{
    Memory memory;
    Store root = open(memory, 100);
    assert(store_mesh_name_assign(&root, 100, 1) == STORE_OK);
    /* Eight authenticated first joins may arrive in any interleaving; the
     * root's serialized Store commit is the only allocator. */
    for (uint64_t device = 101; device <= 108; ++device)
        assert(store_mesh_name_assign(&root, device,
                                      store_mesh_name_next_index(&root)) == STORE_OK);
    for (uint64_t device = 101; device <= 108; ++device)
        assert(store_mesh_name_assign(&root, device,
                                      static_cast<uint16_t>(device - 99)) == STORE_DUPLICATE);
    assert(root.record.mesh_name_count == 9); /* Root plus eight live mesh nodes. */
    assert(store_mesh_name_next_index(&root) == 10);
}

void test_root_change_and_partition_rules()
{
    const StoreMeshNameEntry common[] = {{1, 1}, {2, 2}};
    Memory old_root_memory;
    Store old_root = open(old_root_memory, 1);
    assert(store_mesh_name_merge(&old_root, nullptr, common, 2) == STORE_OK);

    Memory new_root_memory;
    Store new_root = open(new_root_memory, 2);
    assert(store_mesh_name_adopt(&new_root, common, 2) == STORE_OK);
    assert_name(new_root, "KatmedESP-002", 2);
    assert(store_mesh_name_assign(&new_root, 3, store_mesh_name_next_index(&new_root)) == STORE_OK);
    assert(store_mesh_name_has_device(&new_root, 3));

    const StoreMeshNameEntry different_index[] = {{1, 1}, {2, 2}, {4, 4}};
    assert(store_mesh_name_merge(&new_root, nullptr, different_index, 3) == STORE_OK);
    assert(store_mesh_name_has_device(&new_root, 4));

    const StoreMeshNameEntry same_index_collision[] = {{5, 2}};
    assert(store_mesh_name_merge(&new_root, nullptr, same_index_collision, 1) == STORE_OK);
    assert(!store_durable_divergent(&new_root));
    assert(new_root.record.mesh_names[new_root.record.mesh_name_count-1].device==5);
    assert(new_root.record.mesh_names[new_root.record.mesh_name_count-1].index==5);
    Store recovered_root=open(new_root_memory,2);
    assert(!store_durable_divergent(&recovered_root));
    assert(store_mesh_name_next_index(&recovered_root)==6);
    assert(store_mesh_name_assign(&recovered_root,6,
                                  store_mesh_name_next_index(&recovered_root))==STORE_OK);

    /* A returning partition leaf accepts the current authenticated root's
     * canonical map, replacing its old colliding assignment and resuming. */
    Memory partition_memory;
    Store partition = open(partition_memory, 5);
    assert(store_mesh_name_assign(&partition,5,2)==STORE_OK);
    assert(store_mesh_name_adopt(&partition,recovered_root.record.mesh_names,
                                 recovered_root.record.mesh_name_count)==STORE_OK);
    assert_name(partition,"KatmedESP-005",5);
    assert(!store_durable_divergent(&partition));

    Memory rejoin_memory;
    Store rejoin = open(rejoin_memory, 2);
    assert(store_mesh_name_adopt(&rejoin, recovered_root.record.mesh_names,
                                 recovered_root.record.mesh_name_count) == STORE_OK);
    assert_name(rejoin, "KatmedESP-002", 2);
    assert(store_mesh_name_assign(&rejoin,7,store_mesh_name_next_index(&rejoin))==STORE_OK);
    assert(store_mesh_name_has_device(&rejoin,7));
}

void test_index_1000_and_resource_exhaustion()
{
    Memory memory;
    Store root=open(memory,1);
    const StoreMeshNameEntry near_boundary[]={{1,999}};
    assert(store_mesh_name_merge(&root,nullptr,near_boundary,1)==STORE_OK);
    assert(store_mesh_name_next_index(&root)==1000);
    assert(store_mesh_name_assign(&root,2,1000)==STORE_OK);
    assert(std::strcmp(store_mesh_name(&root),"KatmedESP-999")==0);
    Memory leaf_memory;
    Store leaf=open(leaf_memory,3);
    const StoreMeshNameEntry map[]={{1,999},{2,1000},{3,1001}};
    assert(store_mesh_name_adopt(&leaf,map,3)==STORE_OK);
    assert_name(leaf,"KatmedESP-1001",1001);
    Store reopened=open(memory,1);
    assert(store_mesh_name_next_index(&reopened)==1001);
    assert(store_mesh_name_assign(&reopened,4,1001)==STORE_OK);

    Memory exhausted_memory;
    Store exhausted=open(exhausted_memory,55);
    const StoreMeshNameEntry last_index[]={{55,65535}};
    assert(store_mesh_name_merge(&exhausted,nullptr,last_index,1)==STORE_OK);
    assert(store_mesh_name_next_index(&exhausted)==65536);
    assert(store_mesh_name_assign(&exhausted,56,1)==STORE_STALE);
    Store exhausted_reopened=open(exhausted_memory,55);
    assert(store_mesh_name_next_index(&exhausted_reopened)==65536);

    Memory bounded_memory;
    Store bounded=open(bounded_memory,1000);
    for(uint16_t i=1;i<=KATMED_MESH_NAME_MAX_ENTRIES;++i)
        assert(store_mesh_name_assign(&bounded,1000u+i,i)==STORE_OK);
    assert(store_mesh_name_assign(&bounded,9999,65)==STORE_FULL);
    assert(store_mesh_name_next_index(&bounded)==65);
}

void test_name_map_wire_round_trip()
{
    const StoreMeshNameEntry input[] = {{1, 1}, {2, 2}, {9, 1000}, {10, 65535}};
    uint8_t bytes[KATMED_MESH_PAYLOAD_MAX] = {0};
    uint16_t length = 0;
    assert(katmed_mesh_encode_name_map(input, 4, bytes, sizeof(bytes), &length));
    StoreMeshNameEntry output[KATMED_MESH_NAME_MAX_ENTRIES] = {};
    uint8_t count = 0;
    assert(katmed_mesh_decode_name_map(bytes, length, output, &count));
    assert(count == 4);
    for (unsigned i=0; i<count; ++i)
        assert(output[i].device == input[i].device && output[i].index == input[i].index);
    assert(!katmed_mesh_decode_name_map(bytes, length - 1, output, &count));
    assert(!katmed_mesh_encode_name_map(input, 0, bytes, sizeof(bytes), &length));
}
}

int main()
{
    test_fresh_assignment_and_retry();
    test_commit_before_after_and_unknown();
    test_concurrent_first_joins();
    test_root_change_and_partition_rules();
    test_index_1000_and_resource_exhaustion();
    test_name_map_wire_round_trip();
    std::puts("persistent mesh device names: fresh, retry, reboot, commit uncertainty, root change, merge and collision OK");
}
